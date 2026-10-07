#ifndef TASKHANDEL_H
#define TASKHANDEL_H
#include "stdint.h"



void Dog_task();
void MQTT();
void AT();
void UI();
void Modbus();
void page_log_refresh(void);
// 温湿度数据结构体

typedef struct {
    float temp;   // 温度
    float shi;    // 湿度
} Data_t;



#endif
