#include "esp_at.h"
#include "command.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>
#include "stdlib.h"
#include "libemqtt.h"
#include "pal.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "flash.h"
#include "Taskhandel.h"

extern uint8_t a[256];

/* ESP8266 uart1 波特率115200 DMA空闲中断*/
//+++正确退出透传后 下一条语句就不会输出error

/* 两次软复位的最小间隔：避免"复位 -> 必然失败 -> 再复位"的自激循环 */
#define ESP_RESET_MIN_INTERVAL_MS  60000U
/* AT+RST 后等待模块重启并重新关联热点 */
#define ESP_RESET_WAIT_MS          6000U

/* 探测模块是否处于命令模式（能响应 AT） */
static int ESP_ProbeCmdMode(uint32_t timeout_ms)
{
    return ESP_SendCmd_OK("AT\r\n", timeout_ms) == ESP_OK;
}

/*
 * 把 ESP8266 恢复到已知可用的命令模式。
 * ESP8266 的状态独立于 STM32：MCU 复位和重新烧录都不会影响它，
 * 只有 AT+RST 软复位（或整板断电）才能清掉卡死的透传模式 / 僵尸 TCP 连接。
 */
static void ESP_RecoverToCmdMode(void)
{
    static uint32_t last_reset_tick = 0;
    static uint8_t  ever_reset = 0;

    /* 1. 能回 AT 说明本来就在命令模式。
     *    此时绝不能发 "+++"：它不是转义序列而只是三个普通字符，
     *    会留在模块命令行缓冲里，把随后的指令变成 "+++AT" 而返回 ERROR。*/
    if (ESP_ProbeCmdMode(1000)) return;

    /* 2. 无响应：可能正处于透传模式，发 "+++" 退出后再探测 */
    HAL_UART_Transmit(&huart1, (uint8_t *)"+++\r\n", 3, 100);
    vTaskDelay(1000);
    if (ESP_ProbeCmdMode(1000)) return;

    /* 3. 仍无响应：软复位模块（带节流，防止复位风暴） */
    if (ever_reset && (HAL_GetTick() - last_reset_tick) < ESP_RESET_MIN_INTERVAL_MS)
    {
        Log_Write(LOG_WARN, "ESP reset throttled");
        return;
    }
    ever_reset = 1;
    last_reset_tick = HAL_GetTick();

    HAL_UART_Transmit(&huart1, (uint8_t *)"AT+RST\r\n", 8, 100);
    Log_Write(LOG_WARN, "ESP soft reset");
    vTaskDelay(pdMS_TO_TICKS(ESP_RESET_WAIT_MS));
    (void)ESP_ProbeCmdMode(2000);
}

/* ==================== 连接 ESP8266 并完成 MQTT 会话 ====================
 * 首连 (MQTT_Init) 与断线重连 (RE_MQTT_Init) 共用本函数，保证两条路径行为一致。
 * 返回 1 成功（已订阅主题），0 失败。
 */
static int MQTT_ConnectOnce(void)
{
    /* 1. 把模块恢复到命令模式，否则后面的 AT 指令会被当成透传数据发给 socket */
    ESP_RecoverToCmdMode();
    vTaskDelay(500);

    /* 2. 等待 ESP8266 就绪：刚复位或刚上电时模块可能尚未启动完成 */
    int ready = 0;
    for (int i = 0; i < 5; i++)
    {
        if (ESP_SendCmd_OK("AT\r\n", 1000) == ESP_OK) { ready = 1; break; }
        vTaskDelay(300);
    }
    if (!ready) { Log_Write(LOG_WARN, "AT no response"); return 0; }

    /* 3. 关闭可能残留的旧连接，避免 CIPSTART 返回 ALREADY CONNECTED/ERROR */
    ESP_SendCmd_OK("AT+CIPCLOSE\r\n", 1000);

    /* 4. STA 模式 + 连接 WiFi */
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
    char wifi_cmd[64];
    sprintf(wifi_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    if (ESP_SendCmd(wifi_cmd, "WIFI GOT IP", 5000) != ESP_OK)
    {
        Log_Write(LOG_WARN, "CWJAP fail");   /* 不返回：模块可能已自动连上热点 */
    }
    vTaskDelay(500);

    /* 5. 建立到巴法云的 TCP 连接。
     * 期望应答必须是 "OK" 而不是 "CONNECT"：模块里若残留旧连接，
     * 它回的是 "ALREADY CONNECTED"（含子串 CONNECT）会被误判成建连成功，
     * 于是每次重试都复用在一条死 socket 上，永远拿不到 CONNACK。
     * 成功路径 CONNECT\r\n\r\nOK 用 "OK" 同样能匹配，
     * "ALREADY CONNECTED\r\n\r\nERROR" 则会被正确判为失败。*/
    char tcp_cmd[64];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", BEMFA_BROKER, BEMFA_PORT);
    if (ESP_SendCmd(tcp_cmd, "OK", 5000) != ESP_OK)
    {
        /* 大概率是残留的僵尸连接：关掉再重试一次 */
        ESP_SendCmd_OK("AT+CIPCLOSE\r\n", 1000);
        vTaskDelay(1000);
        if (ESP_SendCmd(tcp_cmd, "OK", 5000) != ESP_OK)
        {
            Log_Write(LOG_WARN, "CIPSTART fail");
            return 0;
        }
    }
    vTaskDelay(500);

    /* 6. 进入透传模式，准备发送 MQTT 报文 */
    ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
    ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
    vTaskDelay(500);
    RingBuf_Clear();

    /* 7. 发送 MQTT CONNECT 报文（客户端 ID = 巴法云私钥） */
    uint8_t connect_packet[48] = {
        0x10, 0x2E,                    // 固定报头
        0x00, 0x06,                    // 协议名长度 = 6
        0x4D,0x51,0x49,0x73,0x64,0x70, // 协议名: MQIsdp (MQTT v3.1)
        0x03, 0x02, 0x00,0x78,         // 协议版本 + 连接标志 + 保活时间
        0x00, 0x20,                    // 客户端 ID 长度 = 32
    };
    memcpy(&connect_packet[16], BEMFA_UID, sizeof(BEMFA_UID) - 1);
    HAL_UART_Transmit(&huart1, connect_packet, sizeof(connect_packet), 5000);
    vTaskDelay(300);

    /* 8. 等待 CONNACK：0x20 0x02 xx 0x00 表示服务端接受连接 */
    uint8_t connack[4];
    int ret = pal_tcp_recv_raw(0, connack, 4, 3000);
    if (!(ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 && connack[3] == 0x00))
    {
        Log_Write(LOG_WARN, "no CONNACK");
        return 0;
    }

    /* 9. 订阅主题 */
    uint8_t sub_packet[] = {
        0x82, 0x0A,                    // 固定报头
        0x00, 0x01,                    // 可变报头  报文标识符 Packet ID = 1
        0x00, 0x05,                    // 有效载荷  主题名长度 = 5
        0x00, 0x00, 0x00, 0x00, 0x00,  // 主题名：由 TOPIC_PUB 填充
        0x01                           // 订阅 QoS 等级 = 1
    };
    memcpy(&sub_packet[6], TOPIC_PUB, sizeof(TOPIC_PUB) - 1);
    HAL_UART_Transmit(&huart1, sub_packet, sizeof(sub_packet), 5000);
    vTaskDelay(300);

    return 1;
}

/* 首次连接 */
void MQTT_Init(void)
{
    if (MQTT_ConnectOnce())
    {
        g_mqtt_connected = 1;
        Log_Write(LOG_INFO, "CONNECT FINISH");
    }
    else
    {
        g_mqtt_connected = 0;
        Log_Write(LOG_WARN, "CONNECT FAIL");
    }
}

/* 断线重连：复用与首连完全相同的连接流程 */
void RE_MQTT_Init(void)
{
    if (MQTT_ConnectOnce())
    {
        g_mqtt_connected = 1;
        Log_Write(LOG_INFO, "reconnect success");
    }
    else
    {
        g_mqtt_connected = 0;
    }
}

// ==================== MQTT 心跳包 ====================
void MQTT_SendPing(void)
{
  uint8_t ping[] = {0xC0, 0x00};
  HAL_UART_Transmit(&huart1, ping, 2, 2000);
}

/**
 * @brief  透传模式下从 ESP8266 接收原始 TCP 数据
 * @param  sock       未使用（保留参数，兼容之前的接口设计）
 * @param  buf        接收缓冲区指针
 * @param  len        期望接收的字节数
 * @param  timeout_ms 超时时间（毫秒）
 * @return 实际接收到的字节数，超时或失败返回 -1
 */

int pal_tcp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms)
{
    uint32_t start = HAL_GetTick();
    int received = 0;
    while (received < len) {
        if (HAL_GetTick() - start > timeout_ms) {
            return -1;
        }
        uint8_t ch;
        while (RingBuf_Read(&ch) && received < len) {
            buf[received++] = ch;
        }
        //HAL_Delay(1); // 避免CPU空转
				vTaskDelay(1);
    }
    return received;
}


