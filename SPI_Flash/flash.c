#include "flash.h"
#include "bsp_flash.h"
#include <string.h>
#include <stdio.h>
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pal.h"

// ===================== 新增：Flash 全局互斥锁 =====================
SemaphoreHandle_t flash_mutex = NULL;

/* Flash 临界区保护
 * 1) 锁只加在 flash.c 对外接口的最外层，内部实现一律调用 _nolock 版本；
 *    禁止嵌套加锁：Log_Write 加锁后只能调 flash_part_write_nolock()，
 *    若去调公开接口会二次加锁自锁死（这就是原来锁被注释掉的原因）。
 * 2) 加锁顺序约定：uart1_mutex -> flash_mutex，反向获取会死锁。
 * 3) 持 flash_mutex 期间禁止调用 MQTT_PublishJson()：它内部有阻塞发送 + 500ms 延时，
 *    会把等锁的 UI 任务卡住。*/
static void flash_lock(void)
{
    if(flash_mutex != NULL)
        xSemaphoreTake(flash_mutex, portMAX_DELAY);
}

static void flash_unlock(void)
{
    if(flash_mutex != NULL)
        xSemaphoreGive(flash_mutex);
}

/* 必须在 FlashCache_Init() 之前调用
 * （原来是 main.c 里创建，比 FlashCache_Init 还晚，上电时锁根本还不存在）*/
void Flash_Mutex_Init(void)
{
    if(flash_mutex == NULL)
        flash_mutex = xSemaphoreCreateMutex();
}

// ===================== 新增：日志区写入偏移（循环写标记）=====================
 uint32_t log_write_offset = 0U;						//  下一条要写入的空白地址，最后一条有效日志在它前面(日志写指针 为空白地址)  日志偏移地址
static uint32_t log_erased_upto = 0U;					//  日志区内已擦除到的偏移（扇区对齐），回绕后归零



extern UART_HandleTypeDef huart1;

//缓存 结构体对齐大小后为128字节
typedef struct {
    uint32_t magic;          // 魔术字：标记条目是否有效
    uint32_t timestamp;     // 时间戳
    uint16_t data_len;      // 有效数据长度
    uint8_t  data[CACHE_ENTRY_SIZE - 12]; // 数据区
} CacheEntry_t;

static uint32_t write_addr = CACHE_START_ADDR;
static uint32_t read_addr  = CACHE_START_ADDR;
static uint8_t  initialized = 0;


/* ==================== 缓存条目内部操作（必须在持有 flash_mutex 时调用）==================== */

static void cache_read_entry(CacheEntry_t *e, uint32_t addr)
{
    SPI_FLASH_BufferRead((uint8_t *)e, addr, sizeof(CacheEntry_t));
}

/*
 * 断网补传：把读指针推进到下一条"待补传"(CACHE_MAGIC_PENDING)条目。
 * 必须能跳过"已逻辑擦除"(0x00000000) 的槽 —— 否则重启后扫描会在第一个已补传的槽上停住，
 * 后面没补传的数据就再也读不到了。
 * 返回 1 = 找到（read_addr 指向它）；0 = 没有待补传数据（read_addr 已对齐写指针）。
 */
static int cache_advance_to_pending(void)
{
    CacheEntry_t e;
    while (read_addr < write_addr) {
        cache_read_entry(&e, read_addr);
        if (e.magic == CACHE_MAGIC_PENDING) return 1;
        read_addr += sizeof(CacheEntry_t);
    }
    read_addr = write_addr;
    return 0;
}

/*
 * 断点续写：定位可写槽位。
 * 乐观假设（本项目前提）：缓存区不会写满 —— 断网期间缓存的数据量远小于 4MB，
 * 联网后就会被补传干净。所以"槽被占用"只可能出现在写指针已经绕回上一圈的场景，
 * 此时整扇区擦掉直接复用即可，不需要逐条读出来判断扇区里是否还压着待补传数据。
 * - 槽是空的 → 直接用（正常顺序写，不产生任何擦除）
 * - 槽被占用 → 整扇区擦除后复用
 * 注意：write_addr 恒为 128 字节对齐，且 4096 % 128 == 0，
 *       所以它必定落在扇区内某条槽的起始处。
 */
static void cache_alloc_slot(void)
{
    CacheEntry_t e;

    if (write_addr + sizeof(CacheEntry_t) > CACHE_END_ADDR) {
        write_addr = CACHE_START_ADDR;                  /* 到分区尾，回绕 */
    }

    cache_read_entry(&e, write_addr);
    if (e.magic == CACHE_MAGIC_EMPTY) {
        return;                                         /* 空槽，直接写，不产生擦除 */
    }

    /* 槽被占用：整扇区擦除后复用 */
    SPI_FLASH_SectorErase(write_addr & ~(CACHE_SECTOR_SIZE - 1U));
}


/**
 * @brief  分区读数据
 * @param  part_base 分区基地址
 * @param  offset    分区内偏移
 * @param  buf       接收缓冲区
 * @param  len       读取长度
 * @retval 0成功 1失败
 */
/* 内部无锁实现：调用者必须已持有 flash_mutex（锁统一加在对外接口最外层） */
static uint8_t flash_part_read_nolock(uint32_t part_base, uint32_t offset, uint8_t *buf, uint16_t len)
{
    if(buf == NULL) return 1;
    uint32_t real_addr = part_base + offset;			//分区基地址 + 分区内偏移 = Flash 真实物理地址

    SPI_FLASH_BufferRead(buf, real_addr, len);				// 调用底层硬件读

    return 0;
}

/**
 * @brief  分区写数据
 * @param  part_base 分区基地址
 * @param  offset    分区内偏移
 * @param  buf       待写数据
 * @param  len       写入长度
 * @retval 0成功 1失败
 */
/* 内部无锁实现：调用者必须已持有 flash_mutex */
static uint8_t flash_part_write_nolock(uint32_t part_base, uint32_t offset, uint8_t *buf, uint16_t len)
{
    if(buf == NULL) return 1;
    uint32_t real_addr = part_base + offset;

    SPI_FLASH_BufferWrite(buf, real_addr, len);

    return 0;
}



//业务点位Log_Write() → W25Q日志分区存储 → 打开日志页触发page_log_refresh() → 倒序读Flash → LVGL列表(新日志置顶)
/**
入参校验：空字符串 / 锁未创建，直接退出，防止崩溃。
结构体赋值：
时间戳：HAL_GetTick() 记录事件发生时刻。
等级：传入的 LOG_INFO/LOG_WARN/LOG_ERROR。
魔数：固定 0x55AA，标记有效数据。
字符串拷贝：限制长度，防止超出结构体缓冲区导致内存越界。
循环覆盖逻辑
当「当前指针 + 单条日志大小」超过日志分区总大小 → 指针归零，实现循环日志。
工业设备常用方案：日志不手动删，写满自动覆盖旧日志。
调用分区接口写入：上层完全不碰底层地址。
写指针自增，准备写下一条日志。

 */
/*重要 日志写入 全工程统一入口*/
void Log_Write(Log_Level level, const char *str)
{
    if(str == NULL || flash_mutex == NULL) return;

    Log_Obj log_buf = {0};
    uint16_t str_len;

    // 1. 填充日志结构体
    log_buf.time_stamp = HAL_GetTick();  // 系统运行时间戳
    log_buf.level = level;
    log_buf.magic = LOG_MAGIC;           // 标记为有效日志

    // 拷贝日志内容，防止缓冲区溢出
    str_len = strlen(str);
    if(str_len > LOG_CONTENT_LEN - 1U)
        str_len = LOG_CONTENT_LEN - 1U;
    memcpy(log_buf.content, str, str_len);				//拷贝函数的最后一个参数表示 要拷贝的字节数

    flash_lock();

    // 2. 日志区循环判断：写满则回到日志区头部
    if((log_write_offset + LOG_OBJ_SIZE) > LOG_PART_SIZE)
    {
        log_write_offset = 0U;
        log_erased_upto  = 0U;			// 回绕后"已擦除边界"同步归零
    }
    // 3. 覆盖写之前必须先擦除：Flash 只能 1->0，不擦会让新旧数据按位与变成乱码
    if((log_write_offset + LOG_OBJ_SIZE) > log_erased_upto)
    {
        uint32_t end = (log_write_offset + LOG_OBJ_SIZE + CACHE_SECTOR_SIZE - 1U) & ~(CACHE_SECTOR_SIZE - 1U);
        for(uint32_t sec = log_erased_upto; sec < end; sec += CACHE_SECTOR_SIZE)
            SPI_FLASH_SectorErase(LOG_PART_ADDR + sec);
        log_erased_upto = end;
    }
    // 4. 写入日志分区    基地址     偏移地址
    flash_part_write_nolock(LOG_PART_ADDR, log_write_offset, (uint8_t *)&log_buf, LOG_OBJ_SIZE);
    // 5. 偏移自增
    log_write_offset += LOG_OBJ_SIZE;

    flash_unlock();
}


/**
 * @brief  读取单条日志
 * @param  log_offset 日志区内偏移(读指针)
 * @param  log_obj    日志结构体接收缓存
 * @retval 0=有效日志 1=无效/空日志
 */
uint8_t Log_ReadOne(uint32_t log_offset, Log_Obj *log_obj)
{
    if(log_obj == NULL || flash_mutex == NULL) return 1;

    flash_lock();
    flash_part_read_nolock(LOG_PART_ADDR, log_offset, (uint8_t *)log_obj, LOG_OBJ_SIZE);
    flash_unlock();

    // 魔数不匹配 = 无效日志
    if(log_obj->magic != LOG_MAGIC)
        return 1;

    return 0;
}


void Log_EraseAll(void)
{
    uint32_t sec_addr;
    flash_lock();
    //按4KB扇区循环擦除整个日志分区
    for(sec_addr = LOG_PART_ADDR; sec_addr < LOG_PART_ADDR + LOG_PART_SIZE; sec_addr += 4096)
    {
        SPI_FLASH_SectorErase(sec_addr);
    }
    //擦完直接把写指针置0，并把"已擦除边界"标到整片已擦
    log_write_offset = 0;
    log_erased_upto  = LOG_PART_SIZE;
    flash_unlock();
}


/*
上电时扫描 Flash 4MB~8MB 缓存分区，
找到上次断电前最后一条有效缓存数据，
定位下一条数据的写入地址，实现断电续写；
同时初始化读写指针与状态标记，让 Flash 缓存模块正常工作。
*/
void FlashCache_Init(void)
{
	 log_write_offset = 0;  //开机默认从日志分区0地址开始写
	 log_erased_upto  = 0;		//开机从0开始写，覆盖前必须先擦除

    /* 本函数在调度器启动前调用（单任务），不取锁 */
    CacheEntry_t entry;
    uint32_t addr;

    /* 断点续写：顺序扫描，跳过"已逻辑擦除"的槽，定位第一条真正的空槽。
     * 空槽判据必须是 0xFFFFFFFF（物理擦除态），不能用"不等于待补传"来判断。 */
    write_addr = CACHE_END_ADDR;
    for (addr = CACHE_START_ADDR; addr + sizeof(CacheEntry_t) <= CACHE_END_ADDR; addr += sizeof(CacheEntry_t)) {
        cache_read_entry(&entry, addr);
        if (entry.magic == CACHE_MAGIC_EMPTY) {
            write_addr = addr;
            break;
        }
    }
    if (write_addr >= CACHE_END_ADDR) {
        write_addr = CACHE_START_ADDR;              /* 整片写满，回绕复用 */
    }

    /* 断网补传：读指针定位到第一条待补传数据，没有则对齐写指针 */
    read_addr = CACHE_START_ADDR;
    (void)cache_advance_to_pending();

    initialized = 1;
}

// 写入
int FlashCache_Write(const char *json, uint16_t len)
{
    if (!initialized) return -1;
    if (len > CACHE_ENTRY_SIZE - 12) return -2;
    if (flash_mutex == NULL) return -3;

    CacheEntry_t entry;
    memset(&entry, 0xFF, sizeof(entry));        //先铺成物理擦除态，magic 故意先留 0xFFFFFFFF
    entry.timestamp = HAL_GetTick();
    entry.data_len  = len;
    memcpy(entry.data, json, len);

    flash_lock();

    cache_alloc_slot();                         //断点续写：找到一个真正可写的空槽

    /* 阶段 1：写整条。此刻 magic 仍是空槽态(0xFFFFFFFF)，若在阶段 2 之前断电，
     * 只会留下一个"空槽"，不会污染出半截数据。 */
    SPI_FLASH_BufferWrite((uint8_t *)&entry, write_addr, sizeof(CacheEntry_t));

    /* 阶段 2：单独把 magic 置为"待补传"。
     * 0xFFFFFFFF -> 0xDEADBEEF 全程只把 1 清成 0，NOR Flash 物理可写成立。 */
    uint32_t pending = CACHE_MAGIC_PENDING;
    SPI_FLASH_BufferWrite((uint8_t *)&pending, write_addr, sizeof(pending));

    write_addr += sizeof(CacheEntry_t);         //固定128

    flash_unlock();
    return 0;
}

// 读取一条，读完直接销毁（最安全，永不重复）
int FlashCache_ReadAndConsume(char *json, uint16_t *len)
{
    if (!initialized || flash_mutex == NULL) return -1;

    flash_lock();

    int ret = 0;
    if (cache_advance_to_pending()) {
        CacheEntry_t entry;
        cache_read_entry(&entry, read_addr);

        memcpy(json, entry.data, entry.data_len);
        json[entry.data_len] = '\0';
        *len = entry.data_len;

        /* 逻辑擦除：0xDEADBEEF -> 0x00000000 只清位，不需要擦扇区 */
        entry.magic = CACHE_MAGIC_SENT;
        SPI_FLASH_WriteEnable();
        SPI_FLASH_BufferWrite((uint8_t *)&entry, read_addr, sizeof(CacheEntry_t));

        read_addr += sizeof(CacheEntry_t);
    } else {
        ret = -2;                               // 没有待补传数据
    }

    flash_unlock();
    return ret;
}

// 是否有未读数据
// 【 fix 】判断是否有未读数据：只看 read < write，
// ======================================================================
int FlashCache_HasData(void)
{
    if (!initialized || flash_mutex == NULL) return 0;

    flash_lock();
    /* 顺带把读指针推过"已逻辑擦除"的槽，永远指向第一条待补传数据 */
    int has = cache_advance_to_pending();
    flash_unlock();

    return has;
}

/*读指针追上写指针：放弃剩余缓存（仅数据损坏时用）。
 * 不能把 write_addr 一起拉回起点：那样后续写入会直接覆盖还没补传的数据，
 * 且起点残留的旧条目 CACHE_MAGIC_PENDING 还在，下次开机会被当成待补传数据重复补传。*/
void my_clear(void)
{
    flash_lock();
    read_addr = write_addr;
    flash_unlock();
}

// 清空
void FlashCache_Clear(void)
{
    /* 注意：整片擦除耗时很长（1024 个扇区），会一直占着 flash_mutex，
     * UI 的日志刷新会被阻塞，因此本函数只适合开机/停机阶段调用。*/
    flash_lock();
    for (uint32_t addr = CACHE_START_ADDR; addr < CACHE_END_ADDR; addr += CACHE_SECTOR_SIZE) {
        SPI_FLASH_SectorErase(addr);
    }
    write_addr = CACHE_START_ADDR;
    read_addr  = CACHE_START_ADDR;
    initialized = 1;
    flash_unlock();
}

//补传到云端
void MQTT_PublishJson(const char *json)
{
    uint8_t buf[256];
    int pos = 0;
    buf[pos++] = 0x32;                     /* PUBLISH, QoS 1 固定报头 报文类型 */

	uint16_t topic_len = sizeof(TOPIC_PUB)-1;					//主题名长度
  uint16_t msg_len = strlen(json);
	uint16_t rem = 2 + topic_len + 2 + msg_len;				//2字节的主题长度+5字节的主题名+2字节的报文标识符+有效载荷长度

	/*变长剩余长度编码 低位在前 低7位表示数据 第八位(标志位)为1的话表示不止一个字节 小于128一个字节 最多4个字节*/
    do {
        uint8_t b = rem % 128;
        rem /= 128;
        if (rem > 0) b |= 0x80;
        buf[pos++] = b;
    } while (rem > 0);

		/*可变报头 两字节的主题长度 0x00 0x05  5字节的主题名 */
    buf[pos++] = 0;
    buf[pos++] = topic_len;
    memcpy(&buf[pos], TOPIC_PUB, topic_len);
    pos += topic_len;

		/*两字节的报文标识符 0x00 0x01*/
    buf[pos++] = 0;
    buf[pos++] = 1;

		/*有效载荷*/
    memcpy(&buf[pos], json, msg_len);
    pos += msg_len;

    HAL_UART_Transmit(&huart1, buf, pos, 5000);
    vTaskDelay(500);
}

// 【 fix 】标记已发送：只改魔数，不擦除、不乱动指针
// ======================================================================
void FlashCache_MarkSent(void)
{
    if (!initialized || flash_mutex == NULL) return;

    flash_lock();

    /* 依赖 FlashCache_Read 刚把 read_addr 前移一条，必须紧跟其后调用 */
    uint32_t cur = read_addr - sizeof(CacheEntry_t);
    if (cur >= CACHE_START_ADDR) {
        CacheEntry_t entry;
        cache_read_entry(&entry, cur);
        if (entry.magic == CACHE_MAGIC_PENDING) {
            /* 逻辑擦除：0xDEADBEEF -> 0x00000000 全是 1->0，物理可写成立。
             * （原来写 0xFFFFFFFF 需要 0->1，芯片根本写不进去，等于标记从未生效）*/
            entry.magic = CACHE_MAGIC_SENT;
            SPI_FLASH_WriteEnable();			 // SPI Flash 写使能（硬件要求，写操作前必须开启）
            SPI_FLASH_BufferWrite((uint8_t *)&entry, cur, sizeof(CacheEntry_t));			  // 只改魔数，不擦扇区
        }
    }

    flash_unlock();
}

/*从读指针位置读一条缓存数据*/
int FlashCache_Read(char *json, uint16_t *len)
{
    if (!initialized || flash_mutex == NULL) return -1;

    flash_lock();

    int ret = 0;
    if (cache_advance_to_pending()) {           //跳过已补传的槽，找下一条待补传数据
        CacheEntry_t entry;
        cache_read_entry(&entry, read_addr);

        memcpy(json, entry.data, entry.data_len);
        json[entry.data_len] = '\0';
        *len = entry.data_len;
        read_addr += sizeof(CacheEntry_t);//128
    } else {
        ret = -2;                               // 没有待补传数据
    }

    flash_unlock();
    return ret;
}
