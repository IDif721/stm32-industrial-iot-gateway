#ifndef PAL_H
#define PAL_H

#include "main.h"
#include <string.h>
#include "usart.h"
#include "stm32f4xx.h"
#include "stdio.h"
#include "pal_secrets.h"   /* 本地私有配置：WiFi 账号密码 / 巴法云私钥（不入库，见 pal_secrets.h.example）*/




// °Í·¨ÔÆ²ÎÊýÅäÖÃ
#define BEMFA_BROKER "mqtt.bemfa.com"
#define BEMFA_PORT   9501
#define TOPIC_PUB    "stm32"   // Äã´´½¨µÄÖ÷ÌâÃû



typedef struct {
    uint8_t client_id[50];
    uint8_t username[24];
    uint8_t password[24];
    int socket;
    uint32_t keepalive_interval;
    uint8_t clean_session;
    
    // 关键：函数指针成员，用于发送和接收数据
    int (*send)(void* sock, const void *buf, unsigned int len);
    int (*recv)(int sock, uint8_t *buf, int buf_len, int timeout);
    
    void *socket_info; 
} mqtt_broker_handle_t;



int pal_tcp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms);

void MQTT_Init(void);
void RE_MQTT_Init();

int MQTT_SendTempHumi(float temp, float humi);
void MQTT_SendPing(void);

void MQTT_ReConnect();
#endif
