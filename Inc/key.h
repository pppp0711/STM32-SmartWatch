/**
  ******************************************************************************
  * @file           : key.h
  * @brief          : 按键扫描模块
  * @note           : 4按键(MODE/FUNC/UP/DOWN), 支持短按/长按检测
  ******************************************************************************
  */
#ifndef __KEY_H
#define __KEY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 按键引脚定义 (与main.h中一致) */
#define KEY_MODE_PIN    KEY_MODE_Pin
#define KEY_MODE_PORT   KEY_MODE_GPIO_Port
#define KEY_FUNC_PIN    KEY_PWR_Pin       /* 功能键使用PWR标签的引脚 */
#define KEY_FUNC_PORT   KEY_PWR_GPIO_Port
#define KEY_UP_PIN      KEY_UP_Pin
#define KEY_UP_PORT     KEY_UP_GPIO_Port
#define KEY_DOWN_PIN    KEY_DOWN_Pin
#define KEY_DOWN_PORT   KEY_DOWN_GPIO_Port

/* 按键编号 */
#define KEY_NONE        0
#define KEY_MODE        1
#define KEY_FUNC        2
#define KEY_UP          3
#define KEY_DOWN        4

/* 按键事件类型 */
#define KEY_EVENT_NONE      0x00
#define KEY_EVENT_SHORT     0x01  /* 短按 (按下<1s) */
#define KEY_EVENT_LONG      0x02  /* 长按 (按下>3s) */
#define KEY_EVENT_DOUBLE    0x03  /* 双击 */
#define KEY_EVENT_HOLD      0x04  /* 持续按住 */

/* 按键消抖时间 (ms) */
#define KEY_DEBOUNCE_MS     20
/* 长按判定时间 (ms) */
#define KEY_LONG_PRESS_MS   3000
/* 双击间隔 (ms) */
#define KEY_DOUBLE_CLICK_MS 500

/* 按键状态 */
typedef struct {
    uint8_t  keyNum;       /* 按键编号 */
    uint8_t  event;        /* 事件类型 */
    uint32_t pressTime;    /* 按下持续时间(ms) */
} KeyEvent_t;

/* 函数声明 */
void KEY_Init(void);
uint8_t KEY_Scan(void);
KeyEvent_t KEY_GetEvent(void);
uint8_t KEY_IsPressed(uint8_t keyNum);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H */
