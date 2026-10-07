#ifndef MODBUS_H
#define MODBUS_H
#include "stdint.h"
#include "stm32f4xx.h"

#define RS485_TX  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET)
#define RS485_RX  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET)


int Modbus_Get_Temp(float *temp,float *shi);

uint16_t CRC_16_MODBUS(uint8_t *data,uint8_t len,uint8_t flag);






//单从机配置结构体
typedef struct
{
    uint8_t  dev_addr;    //从机地址
    uint8_t  func;       //功能码
    uint16_t reg_start;  //起始寄存器
    uint16_t reg_num;    //寄存器个数
    uint16_t err_cnt;    //连续失败次数
    float    val1;       //数据1
    float    val2;       //数据2
}ModDev_t;

extern ModDev_t dev_list[];
extern uint8_t dev_cnt;

//通用读寄存器接口（替换原来固定地址Modbus_Get_Temp）
uint8_t Modbus_Read_Dev(uint8_t addr,uint8_t func,uint16_t reg,uint16_t regcnt,uint16_t *buf);

//整表轮询入口
void Modbus_Poll_AllDev(void);

void Modbus_Upload_Json(void);

void MQTT_SendAllDev_NoWait(void);

#endif
