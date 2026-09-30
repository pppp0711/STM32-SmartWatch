/**
  ******************************************************************************
  * @file           : key.c
  * @brief          : 按键扫描实现
  * @note           : 4个独立按键, 上拉输入, 按下为低电平
  *                    短按 < 3s, 长按 >= 3s
  ******************************************************************************
  */
#include "key.h"

/* 按键状态机 */
typedef enum {
    KEY_STATE_IDLE = 0,
    KEY_STATE_DEBOUNCE,
    KEY_STATE_PRESSED,
    KEY_STATE_WAIT_RELEASE
} KeyState_t;

/* 每个按键的状态跟踪 */
static struct {
    KeyState_t state;
    uint32_t   pressTick;
    uint32_t   releaseTick;
    uint8_t    lastPinState;
    uint8_t    eventPending;
} keyInfo[5];  /* 索引1~4对应KEY_MODE~KEY_DOWN */

/* 按键读取宏 */
#define READ_KEY_MODE()   (HAL_GPIO_ReadPin(KEY_MODE_PORT, KEY_MODE_PIN) == GPIO_PIN_RESET)
#define READ_KEY_FUNC()   (HAL_GPIO_ReadPin(KEY_FUNC_PORT, KEY_FUNC_PIN) == GPIO_PIN_RESET)
#define READ_KEY_UP()     (HAL_GPIO_ReadPin(KEY_UP_PORT, KEY_UP_PIN) == GPIO_PIN_RESET)
#define READ_KEY_DOWN()   (HAL_GPIO_ReadPin(KEY_DOWN_PORT, KEY_DOWN_PIN) == GPIO_PIN_RESET)

/**
  * @brief  读取指定按键的当前状态
  * @return 1=按下, 0=释放
  */
static uint8_t ReadKeyPin(uint8_t keyNum)
{
    switch (keyNum)
    {
        case KEY_MODE: return READ_KEY_MODE();
        case KEY_FUNC: return READ_KEY_FUNC();
        case KEY_UP:   return READ_KEY_UP();
        case KEY_DOWN: return READ_KEY_DOWN();
        default:       return 0;
    }
}

/**
  * @brief  按键初始化
  */
void KEY_Init(void)
{
    uint8_t i;
    for (i = 0; i < 5; i++)
    {
        keyInfo[i].state = KEY_STATE_IDLE;
        keyInfo[i].pressTick = 0;
        keyInfo[i].releaseTick = 0;
        keyInfo[i].lastPinState = 0;
        keyInfo[i].eventPending = 0;
    }
}

/**
  * @brief  检查是否有按键按下 (仅检测, 不做消抖)
  * @return 按键编号, 0=无按下
  */
uint8_t KEY_Scan(void)
{
    if (READ_KEY_MODE()) return KEY_MODE;
    if (READ_KEY_FUNC()) return KEY_FUNC;
    if (READ_KEY_UP())   return KEY_UP;
    if (READ_KEY_DOWN()) return KEY_DOWN;
    return KEY_NONE;
}

/**
  * @brief  判断指定按键是否处于按下状态
  */
uint8_t KEY_IsPressed(uint8_t keyNum)
{
    if (keyNum < KEY_MODE || keyNum > KEY_DOWN) return 0;
    return ReadKeyPin(keyNum);
}

/**
  * @brief  获取按键事件 (带状态机的完整消抖处理)
  * @note   每10ms调用一次, 返回完整的按键事件
  *         在FreeRTOS任务中周期性调用
  */
KeyEvent_t KEY_GetEvent(void)
{
    KeyEvent_t event = {KEY_NONE, KEY_EVENT_NONE, 0};
    uint8_t i, pinState;
    uint32_t now = HAL_GetTick();

    for (i = KEY_MODE; i <= KEY_DOWN; i++)
    {
        pinState = ReadKeyPin(i);

        switch (keyInfo[i].state)
        {
            case KEY_STATE_IDLE:
                if (pinState)
                {
                    keyInfo[i].state = KEY_STATE_DEBOUNCE;
                    keyInfo[i].pressTick = now;
                }
                break;

            case KEY_STATE_DEBOUNCE:
                if (pinState)
                {
                    /* 消抖完成 */
                    if (now - keyInfo[i].pressTick >= KEY_DEBOUNCE_MS)
                    {
                        keyInfo[i].state = KEY_STATE_PRESSED;
                    }
                }
                else
                {
                    /* 抖动, 回到空闲 */
                    keyInfo[i].state = KEY_STATE_IDLE;
                }
                break;

            case KEY_STATE_PRESSED:
                if (!pinState)
                {
                    /* 按键释放 */
                    uint32_t pressDuration = now - keyInfo[i].pressTick;
                    if (pressDuration < KEY_LONG_PRESS_MS)
                    {
                        /* 短按事件 */
                        event.keyNum = i;
                        event.event = KEY_EVENT_SHORT;
                        event.pressTime = pressDuration;
                    }
                    else
                    {
                        /* 长按事件 */
                        event.keyNum = i;
                        event.event = KEY_EVENT_LONG;
                        event.pressTime = pressDuration;
                    }
                    keyInfo[i].state = KEY_STATE_IDLE;
                    keyInfo[i].releaseTick = now;
                    return event;  /* 一次只返回一个事件 */
                }
                else if (now - keyInfo[i].pressTick >= KEY_LONG_PRESS_MS)
                {
                    /* 持续按住超过3秒, 产生长按事件 */
                    keyInfo[i].state = KEY_STATE_WAIT_RELEASE;
                    event.keyNum = i;
                    event.event = KEY_EVENT_LONG;
                    event.pressTime = now - keyInfo[i].pressTick;
                    return event;
                }
                break;

            case KEY_STATE_WAIT_RELEASE:
                if (!pinState)
                {
                    keyInfo[i].state = KEY_STATE_IDLE;
                }
                break;
        }
    }

    return event;
}
