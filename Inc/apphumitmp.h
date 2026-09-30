/**
  ******************************************************************************
  * @file           : apphumitmp.h
  * @brief          : AHT20 温湿度传感器驱动
  * @note           : I2C2 (PB10-SCL, PB11-SDA), 地址 0x38
  *                    温度: -40~85°C, ±0.3°C
  *                    湿度: 0~100%RH, ±2%RH
  ******************************************************************************
  */
#ifndef __APPHUMITMP_H
#define __APPHUMITMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "i2c.h"

/* AHT20 I2C地址 */
#define AHT20_ADDR          0x70        /* 0x38 << 1 */
#define AHT20_I2C           hi2c2

/* AHT20 命令 */
#define AHT20_CMD_INIT      0xBE        /* 初始化命令 */
#define AHT20_CMD_SOFTRESET 0xBA        /* 软复位 */
#define AHT20_CMD_TRIGGER   0xAC        /* 触发测量 */
#define AHT20_CMD_TRIGGER_D0 0x33       /* 触发测量数据0 */
#define AHT20_CMD_TRIGGER_D1 0x00       /* 触发测量数据1 */

/* AHT20 状态掩码 */
#define AHT20_STATUS_BUSY   0x80        /* 忙标志 */
#define AHT20_STATUS_CAL    0x08        /* 已校准标志 */

/* 温湿度数据结构体 */
typedef struct {
    float temperature;      /* 温度 (°C) */
    float humidity;         /* 湿度 (%RH) */
    uint8_t valid;          /* 数据有效标志 */
} ahtData_t;

/* 函数声明 */
void AHT20_RTOS_Init(void);
uint8_t AHT20_Init(void);
uint8_t AHT20_ReadData(ahtData_t *data);
uint8_t AHT20_SoftReset(void);
uint8_t AHT20_IsCalibrated(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPHUMITMP_H */
