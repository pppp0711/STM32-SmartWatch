/**
  ******************************************************************************
  * @file           : i2c_oled.h
  * @brief          : SSD1306 OLED 128x64 I2C显示屏驱动
  * @note           : I2C1 (PB6-SCL, PB7-SDA), 地址 0x3C
  ******************************************************************************
  */
#ifndef __I2C_OLED_H
#define __I2C_OLED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "i2c.h"
#include <stdint.h>
#include <string.h>

/* OLED 参数 */
#define OLED_I2C        hi2c1
#define OLED_ADDR       0x78    /* 0x3C << 1 */
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      8

/* OLED 复位引脚 (PB14) */
#define OLED_RESET_PORT GPIOB
#define OLED_RESET_PIN  GPIO_PIN_14

/* 显示颜色 */
#define OLED_COLOR_WHITE    1
#define OLED_COLOR_BLACK    0

/* 字体选项 */
#define FONT_6X8        0
#define FONT_8X16       1
#define FONT_12X24      2       /* 大号数字字体 (仅0-9和:) */

/* 函数声明 */
void OLED_Init(void);
void OLED_Clear(void);
void OLED_ClearBuffer(void);
void OLED_Display(void);
void OLED_RefreshGram(void);
void OLED_SetPos(uint8_t x, uint8_t y);
void OLED_ShowChar(uint8_t x, uint8_t y, uint8_t chr, uint8_t size);
void OLED_ShowString(uint8_t x, uint8_t y, const uint8_t *str, uint8_t size);
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t size);
void OLED_ShowFloat(uint8_t x, uint8_t y, float num, uint8_t intLen, uint8_t decLen, uint8_t size);
void OLED_DrawPoint(uint8_t x, uint8_t y, uint8_t color);
void OLED_Fill(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, uint8_t color);
void OLED_DrawBMP(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, const uint8_t *bmp);
void OLED_ShowCN(uint8_t x, uint8_t y, uint8_t index);
void OLED_WriteCmd(uint8_t cmd);
void OLED_WriteData(uint8_t dat);
void OLED_SetContrast(uint8_t value);
void OLED_DisplayOn(void);
void OLED_DisplayOff(void);

/* 大号字体 (12x24) — 仅支持 0-9 和 : */
void OLED_ShowLargeChar(uint8_t x, uint8_t y, uint8_t chr);
void OLED_ShowLargeString(uint8_t x, uint8_t y, const uint8_t *str);

/* UI 辅助绘图函数 */
void OLED_DrawHLine(uint8_t x, uint8_t y, uint8_t w, uint8_t color);
void OLED_DrawVLine(uint8_t x, uint8_t y, uint8_t h, uint8_t color);
void OLED_DrawFrame(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t color);
void OLED_DrawDashedHLine(uint8_t x, uint8_t y, uint8_t w, uint8_t color);

#ifdef __cplusplus
}
#endif

#endif /* __I2C_OLED_H */
