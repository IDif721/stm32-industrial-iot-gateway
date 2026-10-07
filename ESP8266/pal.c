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
void MQTT_Init(void)
{

	
	  HAL_UART_Transmit(&huart1, (uint8_t *)"+++\r\n", 3, 100);
    vTaskDelay(1000);
	

    // 任意合法AT指令，完成模式切换（不操作TCP，安全无断连）
    ESP_SendCmd_OK("AT\r\n", 2000);
    vTaskDelay(200);
	
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
    char wifi_cmd[64];
    sprintf(wifi_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    ESP_SendCmd(wifi_cmd, "WIFI GOT IP", 5000);
		vTaskDelay(500);
	
    // 建立TCP连接
    char tcp_cmd[64];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", BEMFA_BROKER, BEMFA_PORT);
    ESP_SendCmd(tcp_cmd, "CONNECT", 2000);
		vTaskDelay(500);


    // 进入透传模式
    ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
		//启动透传
		ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
    vTaskDelay(500);
    RingBuf_Clear();	

uint8_t connect_packet[48] = {
        0x10, 0x2E,                // 固定报头
	
        0x00,0x06,                 // 协议名长度 = 6 		可变报头
        0x4D,0x51,0x49,0x73,0x64,0x70, // 协议名: MQIsdp (MQTT v3.1)
        0x03, 0x02, 0x00,0x78,     // 协议版本 + 连接标志 + 保活时间
	
        0x00,0x20,                 // 客户端ID长度 = 32     有效载荷
};
memcpy(&connect_packet[16], BEMFA_UID, sizeof(BEMFA_UID) - 1);   // 32字节 客户端唯一ID
				// 串口发送整包
				HAL_UART_Transmit(&huart1, connect_packet, sizeof(connect_packet), 5000);
				vTaskDelay(300);
	
    // 解析CONNACK应答
    uint8_t connack[4];
		int ret = pal_tcp_recv_raw(0, connack, 4, 3000);			//读取缓冲区 对应字节数
    if (ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 && connack[3] == 0x00)
    {     
       
        // 连接成功后 订阅主题
		uint8_t sub_packet[] = {
				0x82, 0x0A,          // 固定报头
			
				0x00, 0x01,          // 可变报头  报文标识符 Packet ID = 1
			
				0x00, 0x05,          // 有效载荷  主题名长度 = 5
				0x00, 0x00, 0x00, 0x00, 0x00, // 主题名：由TOPIC_PUB填充
				0x01                 // 订阅QoS等级 = 1
		};
		memcpy(&sub_packet[6], TOPIC_PUB, sizeof(TOPIC_PUB) - 1);
		HAL_UART_Transmit(&huart1, sub_packet, sizeof(sub_packet), 5000);
		vTaskDelay(300);
		g_mqtt_connected = 1;
		Log_Write(LOG_INFO, "CONNECT FINISH");
				
    } 
    else 
    {
        g_mqtt_connected = 0;
    }
}

void RE_MQTT_Init(void)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)"+++\r\n", 3, 100);
    vTaskDelay(1000);
	for(int i=0;i<2;i++)
	{
		ESP_SendCmd_OK("AT+RST\r\n", 2000);
    vTaskDelay(pdMS_TO_TICKS(1000));
	}
	
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
    char wifi_cmd[64];
    sprintf(wifi_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    ESP_SendCmd(wifi_cmd, "WIFI GOT IP", 5000);


    // 建立TCP连接
    char tcp_cmd[64];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", BEMFA_BROKER, BEMFA_PORT);
    ESP_SendCmd(tcp_cmd, "CONNECT", 5000);


		ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
		ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
    vTaskDelay(500);
    RingBuf_Clear();	

    // MQTT 连接报文
uint8_t connect_packet[48] = {
        0x10, 0x2E,                // 固定报头
	
        0x00,0x06,                 // 协议名长度 = 6 		可变报头
        0x4D,0x51,0x49,0x73,0x64,0x70, // 协议名: MQIsdp (MQTT v3.1)
        0x03, 0x02, 0x00,0x78,     // 协议版本 + 连接标志 + 保活时间
	
        0x00,0x20,                 // 客户端ID长度 = 32     有效载荷
};
memcpy(&connect_packet[16], BEMFA_UID, sizeof(BEMFA_UID) - 1);   // 32字节 客户端唯一ID
				// 串口发送整包
				HAL_UART_Transmit(&huart1, connect_packet, sizeof(connect_packet), 5000);
				vTaskDelay(300);   
  
		        // 解析CONNACK应答
    uint8_t connack[4];
    int ret = pal_tcp_recv_raw(0, connack, 4, 3000);
		    
    // 收到合法的 CONNACK（0x20 0x02 xx 0x00）才算连接成功
    if (ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 && connack[3] == 0x00)
    {
        // 连接成功后 订阅主题
        uint8_t sub_packet[] = {
            0x82, 0x0A,          // 固定报头
            0x00, 0x01,          // 可变报头  报文标识符 Packet ID = 1
            0x00, 0x05,          // 有效载荷  主题名长度 = 5
            0x00, 0x00, 0x00, 0x00, 0x00, // 主题名：由TOPIC_PUB填充
            0x01                 // 订阅QoS等级 = 1
        };
        memcpy(&sub_packet[6], TOPIC_PUB, sizeof(TOPIC_PUB) - 1);
        HAL_UART_Transmit(&huart1, sub_packet, sizeof(sub_packet), 5000);
        vTaskDelay(300);

        g_mqtt_connected = 1;
        connect = 1;
        Log_Write(LOG_INFO, "reconnect success");
        return;
    }

    // 重连失败：保持离线，等 AT 任务下个周期再重试
    g_mqtt_connected = 0;



		


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


