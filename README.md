# 工业边缘网关监控系统 (STM32 Industrial IoT Gateway)

![MCU](https://img.shields.io/badge/MCU-STM32F411CEU6-blue)
![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)
![GUI](https://img.shields.io/badge/GUI-LVGL%208-orange)
![Cloud](https://img.shields.io/badge/Cloud-%E5%B7%B4%E6%B3%95%E4%BA%91%20MQTT-9cf)
![License](https://img.shields.io/badge/license-MIT-brightgreen)

> 基于 **STM32F411 + FreeRTOS + LVGL** 的工业边缘网关：通过 **RS485 / Modbus-RTU** 采集现场温湿度，
> 经 **ESP8266** 以 **MQTT** 上报到巴法云；**断网时本地 SPI Flash 缓存、恢复后自动补传**，
> 并配备 **2.8 寸电阻触摸屏** 做本地实时显示与日志查询。

An STM32F411-based industrial edge gateway with FreeRTOS + LVGL: it reads field data over Modbus-RTU (RS485),
uploads it to Bemfa Cloud via MQTT over an ESP8266, buffers data in an on-board SPI Flash when offline and
re-uploads it automatically after reconnection, with a local touch-screen UI.

---

## 功能特性

- **多任务架构**：基于 FreeRTOS，UI / 采集 / 联网 / 上报 / 看门狗五个任务解耦协作。
- **本地人机界面**：LVGL 8 + 2.8″ 320×240 TFT，实时显示温湿度、运行时间与 MQTT 连接状态，支持中文。
- **现场数据采集**：RS485 总线上的 Modbus-RTU 温湿度变送器，带 CRC16 校验与超时重试。
- **云端接入**：ESP8266（AT 指令）+ MQTT（巴法云 `mqtt.bemfa.com:9501`），心跳保活、断线自动重连。
- **断网缓存与补传**：W25Q64 划分日志区 / 补传缓冲区，离线数据落盘，联网后分批回传（每轮最多 5 条）。
- **日志系统**：运行日志写入 Flash，可在屏幕上翻阅。
- **可靠性与低功耗设计**：独立看门狗（IWDG，约 20 s）、10 s 无触摸自动息屏、串口 DMA + 空闲中断接收。

## 系统架构

```
                 ┌──────────────────── STM32F411CEU6 ────────────────────┐
                 │                                                        │
  Modbus-RTU     │   FreeRTOS Tasks                                       │
  温湿度传感器 ◄──┼── Modbus  ──►  data_queue ──►  MQTT ──► USART1 ──► ESP8266 ──► WiFi ──► 巴法云 MQTT
  (RS485/USART2) │                                    │                                  ▲
                 │                                    ▼                                  │
   2.8" 触摸屏 ◄──┼── UI (LVGL)                SPI Flash (W25Q64)                  手机/云端查看
   (SPI1)        │                            断网缓存 · 日志 · 界面回读                 │
                 │                                    ▲                                  │
                 │                            Dog_task (IWDG)                           │
                 └────────────────────────────────────────────────────────────────────────┘
```

### FreeRTOS 任务

| 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `UI` | AboveNormal | 4096 B | LCD/触摸初始化、LVGL 刷新、界面数据更新、息屏控制 |
| `Modbus` | Normal+1 | 2048 B | 轮询 RS485 上的 Modbus-RTU 设备 |
| `AT` | Normal | 2048 B | WiFi 连接、MQTT 握手、10 s 心跳检测与断线重连 |
| `MQTT` | Normal | 2048 B | 实时数据上报、历史缓存补传、离线数据落盘 |
| `Dog_task` | AboveNormal+2 | 2048 B | 每 3 s 喂一次独立看门狗（硬件溢出约 20 s） |

## 硬件清单

| 部件 | 型号 / 说明 |
|---|---|
| 主控 | STM32F411CEU6（100 MHz，HSE 25 MHz，LSE 32.768 kHz 供 RTC） |
| 显示屏 | 2.8″ SPI TFT，320×240（ILI9341 初始化序列） |
| 触摸 | 电阻触摸（XPT2046，与 LCD 共用 SPI1） |
| 无线模块 | ESP8266（AT 固件，回传 UART 115200） |
| 现场总线 | RS485（MAX485 类收发器）＋ Modbus-RTU 温湿度变送器 |
| 存储 | W25Q64 SPI Flash（8 MB，SPI2） |

## 引脚接线

| 功能 | 引脚 | 备注 |
|---|---|---|
| SPI1 SCK / MISO / MOSI | PA5 / PA6 / PA7 | LCD 与触摸共用，软件片选 |
| LCD CS / DC(RS) / RST / BLK | PB6 / PB7 / PA11 / PA8 | BLK 高电平点亮背光 |
| 触摸 CS | PB10 | XPT2046 |
| ESP8266 — USART1 TX / RX | PA9 / PA10 | 115200，DMA 收发 |
| RS485 — USART2 TX / RX | PA2 / PA3 | 9600 8N1 |
| RS485 收发使能 DE | PB0 | |
| SPI2 SCK / MISO / MOSI | PB13 / PB14 / PB15 | W25Q64 |
| SPI Flash CS | PB12 | |
| 状态指示灯 | PB3 / PB4 / PA15 | 在线 / 补传 / 离线指示 |

## 目录结构

```
.
├─ Core/           CubeMX 生成的 HAL 初始化、中断向量、FreeRTOS 配置(含 HAL 时基 TIM4)
├─ code/           LCD 与触摸驱动、任务实现(Taskhandel.c)、UI 相关
├─ model/          Modbus-RTU 采集与 JSON 组包上报
├─ ESP8266/        AT 指令驱动、MQTT 报文、巴法云对接、环形缓冲区(command.c)
├─ SPI_Flash/      W25Q64 驱动(bsp_flash.c)、日志分区与断网缓存补传(flash.c)
├─ Lvgl/           LVGL 8 源码 + 移植层(examples/porting) + 自定义界面(LVGL_myGUI)
├─ Middlewares/    FreeRTOS 内核
├─ Drivers/        STM32F4 HAL 与 CMSIS
├─ MDK-ARM/        Keil MDK 工程(F4project.uvprojx)
└─ F4project.ioc   STM32CubeMX 工程文件
```

## 快速开始

**开发环境**：Keil MDK-ARM V5 + STM32F4xx DFP + ARM Compiler；如需修改外设配置可用 STM32CubeMX 打开 `F4project.ioc`。

```bash
git clone https://github.com/IDif721/stm32-industrial-iot-gateway.git
cd stm32-industrial-iot-gateway
```

### 1. 创建私有配置文件（必须）

WiFi 账号密码与巴法云私钥**不入库**，需要自己创建：

```bash
# Windows
copy ESP8266\pal_secrets.h.example ESP8266\pal_secrets.h
# Linux / macOS
cp ESP8266/pal_secrets.h.example ESP8266/pal_secrets.h
```

然后编辑 `ESP8266/pal_secrets.h`，填入你自己的配置：

```c
#define WIFI_SSID   "你的WiFi名称"
#define WIFI_PASS   "你的WiFi密码"
#define BEMFA_UID   "你的巴法云私钥"   // 巴法云控制台右上角
```

> 缺少 `pal_secrets.h` 会编译报错，这是有意设计：避免把私密凭据提交到仓库。

### 2. 编译与下载

用 Keil 打开 `MDK-ARM/F4project.uvprojx`，编译并下载到目标板即可。

### 3. 云端配置

在 [巴法云控制台](https://cloud.bemfa.com/) 创建一个名为 `stm32` 的 MQTT 主题，设备即向上报数据。

## 通信协议

**Modbus-RTU（从站 0x01，功能码 0x03）**

| 寄存器 | 含义 | 换算 |
|---|---|---|
| 0x0000 (40001) | 湿度 | 值 ÷ 10 → %RH |
| 0x0001 (40002) | 温度 | 值 ÷ 10 → ℃ |

**MQTT / 巴法云**

| 项目 | 值 |
|---|---|
| Broker | `mqtt.bemfa.com:9501` |
| 协议 | MQTT v3.1（`MQIsdp`，Clean Session） |
| ClientID | 巴法云私钥 |
| 上报主题 | `stm32` |
| 报文示例 | `{"temp":23.0,"humi":39.1}` |

## 效果展示

> 图片待补充：把实物照片与界面截图放到 `docs/images/` 后在此引用，例如：
> ```markdown
> ![实物](docs/images/device.jpg)
> ![界面](docs/images/ui.png)
> ```

## 路线图

- [ ] 优化断网重连流程（统一首连与重连逻辑）
- [ ] 支持更多 Modbus 从站设备（继电器、电表等）
- [ ] 增加本地数据曲线显示
- [ ] 支持 OTA 升级

## 许可证

本项目采用 [MIT License](LICENSE)。

## 作者

**IDif721** — https://github.com/IDif721
