/**
  ******************************************************************************
  * @file           : appble.h
  * @brief          : BT04 BLE蓝牙模块驱动
  * @note           : USART1 (PA9-TX, PA10-RX), 9600 baud
  *                    PB0: BT04 KEY/EN 控制引脚
  *                    BT04是BLE4.0模块, 使用AT命令配置
  ******************************************************************************
  */
#ifndef __APPBLE_H
#define __APPBLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "usart.h"

/* BT04 控制引脚 */
#define BLE_KEY_PIN     HC05______KEY_Pin
#define BLE_KEY_PORT    HC05______KEY_GPIO_Port

/* BT04 状态 */
#define BLE_STATE_OFF       0
#define BLE_STATE_AT        1   /* AT命令模式 */
#define BLE_STATE_CONNECTED 2   /* 已连接 */
#define BLE_STATE_DATA      3   /* 数据透传模式 */

/* 蓝牙数据包类型 */
#define BLE_PKT_TEMP        0x01
#define BLE_PKT_HUMI        0x02
#define BLE_PKT_HEART       0x03
#define BLE_PKT_TIME        0x05
#define BLE_PKT_STATUS      0x06

/* 蓝牙远程控制命令 */
#define BLE_CMD_MODE        0   /* MODE键: 切换页面 */
#define BLE_CMD_FUNC        1   /* FUNC键 (短按) */
#define BLE_CMD_UP          2   /* UP键 */
#define BLE_CMD_DOWN        3   /* DOWN键 */
#define BLE_CMD_FUNC_LONG   4   /* FUNC键 (长按) */
#define BLE_CMD_NONE        0xFF

/* 发送缓冲区大小 */
#define BLE_TX_BUF_SIZE     128
#define BLE_RX_BUF_SIZE     128

/* 函数声明 */
void BLE_Init(void);
void BLE_EnterATMode(void);
void BLE_ExitATMode(void);
uint8_t BLE_SendATCommand(const char *cmd, char *resp, uint16_t timeout);
void BLE_SendData(uint8_t type, const char *data);
void BLE_SendFormatted(const char *format, ...);
void BLE_ProcessRxData(uint8_t data);
uint8_t BLE_IsConnected(void);
void BLE_SetConnected(uint8_t connected);
uint8_t BLE_GetCmd(void);
uint8_t BLE_PollRx(uint8_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* __APPBLE_H */
