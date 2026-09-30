/**
  ******************************************************************************
  * @file           : appheart.h
  * @brief          : 心率测量模块 (模拟脉搏传感器)
  * @note           : 使用 ADC1 CH0 (PA0) 采集脉搏波信号
  *                    通过峰值检测算法计算心率 (BPM)
  *                    测量范围: 40~180 BPM
  ******************************************************************************
  */
#ifndef __APPHEART_H
#define __APPHEART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 心率测量状态 */
#define HEART_STATE_IDLE        0   /* 空闲 */
#define HEART_STATE_MEASURING   1   /* 测量中 */
#define HEART_STATE_DONE        2   /* 测量完成 */

/* ADC采样参数 */
#define HEART_SAMPLE_RATE       50  /* 采样频率 (Hz) */
#define HEART_SAMPLE_BUF_SIZE   200 /* 采样缓冲区大小 (4秒数据) */
#define HEART_BEAT_WINDOW       300 /* 心跳检测最小间隔 (ms) */
#define HEART_PEAK_OFFSET       8   /* 峰值检测偏移 (ADC计数) — 小信号用 */

/* 函数声明 */
void HEART_Init(void);
void HEART_StartMeasure(void);
void HEART_StopMeasure(void);
uint8_t HEART_GetBPM(void);
uint8_t HEART_GetState(void);
uint16_t HEART_GetLastADC(void);
void HEART_ProcessSample(uint16_t adcValue);

#ifdef __cplusplus
}
#endif

#endif /* __APPHEART_H */
