/**
  ******************************************************************************
  * @file           : appTask.h
  * @brief          : 智能手表 FreeRTOS 应用任务
  * @note           : 包含全部UI状态机、任务管理、闹钟、秒表等
  *                    界面切换: MODE键循环
  *                    0:时间显示 → 1:时间设置 → 2:闹钟设置 → 3:秒表
  *                    → 4:心率测量 → 5:运动计步 → 6:硬件信息
  ******************************************************************************
  */
#ifndef __APPTASK_H
#define __APPTASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

/* ======================== 界面编号 ======================== */
typedef enum {
    SCREEN_TIME      = 0,   /* 时间显示 (日期/时间/星期/温湿度) */
    SCREEN_ALARM     = 1,   /* 闹钟设置 */
    SCREEN_STOPWATCH = 2,   /* 秒表计时 0.01s */
    SCREEN_HEART     = 3,   /* 心率测量 */
    SCREEN_MAX       = 4
} ScreenID_t;

/* ======================== 全局数据结构 ======================== */
/* 闹钟设置 */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t enable;
} AlarmSetting_t;

/* 秒表状态 */
typedef enum {
    STOPWATCH_IDLE = 0,
    STOPWATCH_RUNNING,
    STOPWATCH_STOPPED,
    STOPWATCH_RESET
} StopwatchState_t;

/* 秒表时间 */
typedef struct {
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint16_t centisecond;   /* 0.01s */
} StopwatchTime_t;

/* 系统模式 */
typedef struct {
    ScreenID_t    currentScreen;
    uint8_t       displayOn;
    uint32_t      lastKeyTick;
    uint32_t      systemTick;
} SystemMode_t;

/* ======================== 外部变量声明 ======================== */
extern SystemMode_t    g_sysMode;
extern AlarmSetting_t  g_alarm;
extern StopwatchTime_t g_swTime;
extern StopwatchState_t g_swState;
/* 编辑模式标志 */
extern uint8_t         g_timeEditMode;   /* 时间编辑模式 */
extern uint8_t         g_timeEditPos;    /* 时间编辑位: 0=年 1=月 2=日 3=星期 4=时 5=分 6=秒 */
extern uint8_t         g_alarmEditMode;  /* 闹钟编辑模式 */
/* 任务/消息队列句柄 */
extern QueueHandle_t   xQueueKeyEvent;
extern QueueHandle_t   xQueueSensorData;
extern QueueHandle_t   xQueueHeartData;
extern TimerHandle_t   xTimerStopwatch;

/* ======================== 函数声明 ======================== */
void appStart(void);

/* 任务函数 */
void vTask_Display(void *pvParameters);
void vTask_KeyScan(void *pvParameters);
void vTask_Time(void *pvParameters);
void vTask_Sensor(void *pvParameters);
void vTask_Bluetooth(void *pvParameters);
void vTask_Stopwatch(void *pvParameters);

/* 界面绘制 */
void DrawTimeScreen(void);
void DrawAlarmScreen(void);
void DrawStopwatchScreen(void);
void DrawHeartScreen(void);
/* 辅助函数 */
void EnterSleepMode(void);
void ExitSleepMode(void);
void ToggleScreen(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPTASK_H */
