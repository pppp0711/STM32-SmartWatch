/**
  ******************************************************************************
  * @file           : apphumitmp.c
  * @brief          : AHT20 温湿度传感器驱动实现
  * @note           : I2C2, 地址0x38, 单次测量约80ms
  *                    温度公式: T = (raw / 2^20) * 200 - 50
  *                    湿度公式: RH = (raw / 2^20) * 100
  ******************************************************************************
  */
#include "apphumitmp.h"
#include "cmsis_os.h"
#include "event_groups.h"
#include "timers.h"

/* ==================== FreeRTOS 事件标志 + 软件定时器 ==================== */
/* AHT20 无中断引脚, 测量完成只能靠定时到点后置位事件标志唤醒任务 */
#define AHT20_EVT_MEASURE_DONE   (1u << 0)

static EventGroupHandle_t xAHT20EventGroup = NULL;
static TimerHandle_t      xAHT20Timer = NULL;

static void AHT20_TimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (xAHT20EventGroup != NULL) {
        xEventGroupSetBits(xAHT20EventGroup, AHT20_EVT_MEASURE_DONE);
    }
}

void AHT20_RTOS_Init(void)
{
    xAHT20EventGroup = xEventGroupCreate();
    xAHT20Timer = xTimerCreate("aht20", pdMS_TO_TICKS(80),
                               pdFALSE, NULL, AHT20_TimerCallback);
}

/* AHT20 初始化状态 */

/**
  * @brief  读取AHT20状态寄存器
  */
static uint8_t AHT20_ReadStatus(void)
{
    uint8_t status;
    HAL_I2C_Master_Receive(&AHT20_I2C, AHT20_ADDR, &status, 1, 10);
    return status;
}

/**
  * @brief  AHT20软复位
  */
uint8_t AHT20_SoftReset(void)
{
    uint8_t cmd = AHT20_CMD_SOFTRESET;
    if (HAL_I2C_Master_Transmit(&AHT20_I2C, AHT20_ADDR, &cmd, 1, 20) != HAL_OK)
    {
        return 0;
    }
    osDelay(20);  /* 等待复位完成 */
    return 1;
}

/**
  * @brief  检查AHT20是否已完成校准
  */
uint8_t AHT20_IsCalibrated(void)
{
    uint8_t status = AHT20_ReadStatus();
    return (status & AHT20_STATUS_CAL) ? 1 : 0;
}

/**
  * @brief  AHT20初始化 (发送初始化命令并等待校准完成)
  * @return 1=成功, 0=失败
  */
uint8_t AHT20_Init(void)
{
    uint8_t cmd[2];
    uint32_t timeout;

    /* 先软复位 */
    AHT20_SoftReset();
    osDelay(40);

    /* 检查芯片是否响应 */
    if (!AHT20_IsCalibrated())
    {
        /* 发送初始化命令 */
        cmd[0] = AHT20_CMD_INIT;
        cmd[1] = 0x08;
        if (HAL_I2C_Master_Transmit(&AHT20_I2C, AHT20_ADDR, cmd, 2, 20) != HAL_OK)
        {
            return 0;
        }
        osDelay(10);

        /* 等待校准完成 (最多500ms) */
        timeout = 500;
        while (timeout--)
        {
            osDelay(1);
            if (AHT20_IsCalibrated())
            {
                return 1;
            }
        }
        return 0;
    }

    return 1;
}

/**
  * @brief  触发AHT20测量并读取温湿度数据
  * @param  data: 输出温湿度数据
  * @return 1=成功, 0=失败
  * @note   测量耗时约80ms
  */
uint8_t AHT20_ReadData(ahtData_t *data)
{
    uint8_t cmd[3];
    uint8_t buf[7];
    uint32_t rawHum, rawTemp;
    uint8_t status;

    if (data == NULL) return 0;

    /* 触发测量 */
    cmd[0] = AHT20_CMD_TRIGGER;
    cmd[1] = AHT20_CMD_TRIGGER_D0;
    cmd[2] = AHT20_CMD_TRIGGER_D1;
    if (HAL_I2C_Master_Transmit(&AHT20_I2C, AHT20_ADDR, cmd, 3, 20) != HAL_OK)
    {
        return 0;
    }

    /* 等待测量完成: 软件定时器80ms后置位事件标志, 唤醒本任务 */
    if (xAHT20EventGroup != NULL && xAHT20Timer != NULL) {
        xTimerStart(xAHT20Timer, 0);
        xEventGroupWaitBits(xAHT20EventGroup, AHT20_EVT_MEASURE_DONE,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
    } else {
        osDelay(80);
    }

    /* 读取状态+数据 (共7字节) */
    if (HAL_I2C_Master_Receive(&AHT20_I2C, AHT20_ADDR, buf, 7, 20) != HAL_OK)
    {
        return 0;
    }

    /* 检查忙标志 */
    status = buf[0];
    if (status & AHT20_STATUS_BUSY)
    {
        return 0;
    }

    /* 计算湿度 (%RH)
     * 湿度 = (raw / 2^20) * 100
     * rawHum = (buf[1] << 12) | (buf[2] << 4) | (buf[3] >> 4)
     */
    rawHum = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
    float hum = (float)rawHum * 100.0f / 1048576.0f;  /* / 2^20 */

    /* 计算温度 (°C)
     * 温度 = (raw / 2^20) * 200 - 50
     * rawTemp = ((buf[3] & 0x0F) << 16) | (buf[4] << 8) | buf[5]
     */
    rawTemp = ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];
    float tmp = (float)rawTemp * 200.0f / 1048576.0f - 50.0f;

    /* 合理性检查 — 通过后才更新, 失败则保留旧数据 */
    if (tmp > -40.0f && tmp < 85.0f &&
        hum >= 0.0f && hum <= 100.0f)
    {
        data->temperature = tmp;
        data->humidity = hum;
        data->valid = 1;
        return 1;
    }

    return 0;
}
