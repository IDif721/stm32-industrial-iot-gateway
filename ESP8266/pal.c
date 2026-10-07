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

/* ==================== 连接 ESP8266 并完成 MQTT 会话 ====================
 * 首连 (MQTT_Init) 与断线重连 (RE_MQTT_Init) 共用本函数，保证两条路径行为一致。
 * 返回 1 成功（已订阅主题），0 失败。
 */
static int MQTT_ConnectOnce(void)
{
    /* 1. 退出透传模式（若模块正在透传，AT 指令不会被识别） */
    HAL_UART_Transmit(&huart1, (uint8_t *)"+++\r\n", 3, 100);
    vTaskDelay(1000);

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

    /* 5. 建立到巴法云的 TCP 连接 */
    char tcp_cmd[64];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", BEMFA_BROKER, BEMFA_PORT);
    if (ESP_SendCmd(tcp_cmd, "CONNECT", 5000) != ESP_OK)
    {
        Log_Write(LOG_WARN, "CIPSTART fail");
        return 0;
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
        connect = 1;
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


