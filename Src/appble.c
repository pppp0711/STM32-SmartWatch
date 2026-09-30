/**
  ******************************************************************************
  * @file           : appble.c
  * @brief          : BT04 BLE蓝牙模块驱动实现
  * @note           : USART1, 9600bps
  *                   BT04 BLE模块默认串口参数: 9600, 8N1
  *                   AT命令模式: KEY引脚拉高, 重新上电或发送特定命令
  *                   数据透传模式: KEY引脚拉低
  ******************************************************************************
  */
#include "appble.h"
#include "gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static uint8_t bleState = BLE_STATE_OFF;
static uint8_t bleConnected = 0;
static uint8_t rxBuf[BLE_RX_BUF_SIZE];
static volatile uint8_t bleCmd = BLE_CMD_NONE;
static uint16_t rxLen = 0;
static char txBuf[BLE_TX_BUF_SIZE];

/* USART1用于蓝牙通信 */
#define BLE_UART huart1

/**
  * @brief  BT04初始化
  */
void BLE_Init(void)
{
    /* EN拉高使能模块 */
    HAL_GPIO_WritePin(BLE_KEY_PORT, BLE_KEY_PIN, GPIO_PIN_SET);
    HAL_Delay(1000);

    /*
     * 以115200发AT测试 (大部分BT04默认115200)
     * 如果手机收到 "AT_115200" 说明TX方向通
     */
    huart1.Init.BaudRate = 115200;
    HAL_UART_Init(&huart1);
    HAL_UART_Transmit(&huart1, (uint8_t*)"AT\r\n", 4, 100);
    HAL_Delay(100);

    /*
     * 以9600发测试
     * 如果手机收到 "AT_9600" 说明9600通
     */
    huart1.Init.BaudRate = 9600;
    HAL_UART_Init(&huart1);
    HAL_UART_Transmit(&huart1, (uint8_t*)"AT\r\n", 4, 100);
    HAL_Delay(100);
    /* 保持9600用于后续通信 */
    huart1.Init.BaudRate = 9600;
    HAL_UART_Init(&huart1);

    /* KEY保持HIGH (AT模式), 串口和BLE都能收发 */
    bleState = BLE_STATE_DATA;
    bleConnected = 0;
    rxLen = 0;
    memset(rxBuf, 0, sizeof(rxBuf));
}

/**
  * @brief  轮询接收一字节 (非阻塞)
  * @return 1=收到数据, 0=无数据
  */
uint8_t BLE_PollRx(uint8_t *ch)
{
    if (__HAL_UART_GET_FLAG(&BLE_UART, UART_FLAG_RXNE))
    {
        *ch = (uint8_t)(BLE_UART.Instance->DR & 0xFF);
        return 1;
    }
    return 0;
}

/**
  * @brief  进入AT命令模式
  * @note   KEY拉高, BT04进入AT模式后波特率固定9600
  */
void BLE_EnterATMode(void)
{
    HAL_GPIO_WritePin(BLE_KEY_PORT, BLE_KEY_PIN, GPIO_PIN_SET);
    HAL_Delay(200);  /* 等待模块切换模式 */
    bleState = BLE_STATE_AT;
}

/**
  * @brief  退出AT命令模式(回到透传)
  */
void BLE_ExitATMode(void)
{
    HAL_GPIO_WritePin(BLE_KEY_PORT, BLE_KEY_PIN, GPIO_PIN_RESET);
    HAL_Delay(200);
    bleState = BLE_STATE_DATA;
}

/**
  * @brief  发送AT命令并等待响应
  * @param  cmd: AT命令字符串 (不加\r\n, 自动添加)
  * @param  resp: 响应缓冲区 (可为NULL)
  * @param  timeout: 超时时间(ms)
  * @return 1=收到OK, 0=超时或失败
  */
uint8_t BLE_SendATCommand(const char *cmd, char *resp, uint16_t timeout)
{
    /* 确保在AT模式 */
    if (bleState != BLE_STATE_AT)
    {
        BLE_EnterATMode();
    }

    /* 发送命令 */
    snprintf(txBuf, BLE_TX_BUF_SIZE, "%s\r\n", cmd);
    HAL_UART_Transmit(&BLE_UART, (uint8_t *)txBuf, strlen(txBuf), 100);

    /* 等待响应 */
    HAL_Delay(timeout);

    /* 检查响应中是否包含OK */
    if (resp != NULL)
    {
        memcpy(resp, (char *)rxBuf, rxLen > BLE_RX_BUF_SIZE ? BLE_RX_BUF_SIZE : rxLen);
    }

    /* 检查是否成功 */
    uint8_t result = (strstr((const char *)rxBuf, "OK") != NULL) ? 1 : 0;

    /* 清空接收缓冲区 */
    rxLen = 0;
    memset(rxBuf, 0, sizeof(rxBuf));

    return result;
}

/**
  * @brief  通过蓝牙发送数据包
  * @param  type: 数据类型
  * @param  data: 数据字符串
  */
void BLE_SendData(uint8_t type, const char *data)
{
    if (bleState != BLE_STATE_DATA) return;

    /* 数据包格式: [TYPE:1byte][DATA][\r\n] */
    snprintf(txBuf, BLE_TX_BUF_SIZE, "T:%d,D:%s\r\n", type, data);
    HAL_UART_Transmit(&BLE_UART, (uint8_t *)txBuf, strlen(txBuf), 100);
}

/**
  * @brief  发送格式化数据
  */
void BLE_SendFormatted(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(txBuf, BLE_TX_BUF_SIZE, format, args);
    va_end(args);

    if (bleState == BLE_STATE_DATA)
    {
        HAL_UART_Transmit(&BLE_UART, (uint8_t *)txBuf, strlen(txBuf), 100);
    }
}

/**
  * @brief  处理蓝牙接收数据 (在USART1中断中调用)
  * @param  data: 接收到的字节
  */
void BLE_ProcessRxData(uint8_t data)
{
    /* 解析远程控制命令: A=切页 B=功能 C=Up D=Down E=长按FUNC */
    switch (data)
    {
        case 'A': case 'a': bleCmd = BLE_CMD_MODE;      rxLen = 0; return;
        case 'B': case 'b': bleCmd = BLE_CMD_FUNC;      rxLen = 0; return;
        case 'C': case 'c': bleCmd = BLE_CMD_UP;        rxLen = 0; return;
        case 'D': case 'd': bleCmd = BLE_CMD_DOWN;      rxLen = 0; return;
        case 'E': case 'e': bleCmd = BLE_CMD_FUNC_LONG; rxLen = 0; return;
    }

    if (rxLen < BLE_RX_BUF_SIZE - 1)
    {
        rxBuf[rxLen++] = data;
        rxBuf[rxLen] = '\0';
        if (data == '\n' && rxLen >= 2)
        {
            if (strstr((const char *)rxBuf, "CONNECTED"))
            {
                bleConnected = 1;
                bleState = BLE_STATE_CONNECTED;
            }
            else if (strstr((const char *)rxBuf, "DISCONNECT"))
            {
                bleConnected = 0;
                bleState = BLE_STATE_DATA;
            }
            if (rxLen > BLE_RX_BUF_SIZE - 4) { rxLen = 0; memset(rxBuf, 0, sizeof(rxBuf)); }
        }
    }
    else { rxLen = 0; memset(rxBuf, 0, sizeof(rxBuf)); }
}

/**
  * @brief  获取并清除蓝牙远程命令
  */
uint8_t BLE_GetCmd(void)
{
    uint8_t cmd = bleCmd;
    bleCmd = BLE_CMD_NONE;
    return cmd;
}

/**
  * @brief  获取蓝牙连接状态
  */
uint8_t BLE_IsConnected(void)
{
    return bleConnected;
}

/**
  * @brief  设置蓝牙连接状态
  */
void BLE_SetConnected(uint8_t connected)
{
    bleConnected = connected;
    if (connected)
        bleState = BLE_STATE_CONNECTED;
    else
        bleState = BLE_STATE_DATA;
}

/**
  * @brief  USART1接收中断回调 (由USART1_IRQHandler直接处理)
  * @note   接收在 stm32f1xx_it.c 的 USART1_IRQHandler 中直接处理
  *          不需要额外的 HAL 回调
  */
