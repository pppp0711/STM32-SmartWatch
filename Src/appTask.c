/**
  ******************************************************************************
  * @file           : appTask.c  智能手表 4页面
  *   MODE键循环: TIME(0)→ALARM(1)→STOPWATCH(2)→HEART(3)→TIME
  *   OLED方向: A1+C8  128×64
  *   界面风格: 统一顶部标题栏 + 中央内容区 + 底部状态栏
  *   时间界面: 12×24 大号数字居中 (占3行)
  ******************************************************************************
  */
#include "appTask.h"
#include "i2c_oled.h"
#include "key.h"
#include "apphumitmp.h"
#include "appheart.h"
#include "appble.h"
#include "rtc.h"
#include "adc.h"
#include "i2c.h"
#include "gpio.h"
#include <stdio.h>
#include <string.h>

/* ==================== 全局变量 ==================== */
SystemMode_t    g_sysMode;
AlarmSetting_t  g_alarm = {7, 0, 0, 0};
StopwatchTime_t g_swTime;
StopwatchState_t g_swState = STOPWATCH_IDLE;

static ahtData_t      g_envData;
static uint8_t        g_heartBPM = 0;
static uint8_t        g_heartOn  = 0;
static RTC_TimeTypeDef g_rtcTime;
static RTC_DateTypeDef g_rtcDate;
static uint8_t        g_blink   = 0;
static uint8_t        g_setPos  = 0;        /* 闹钟设置位: 0=时 1=分 2=秒 */
static uint32_t       g_ringTick = 0;       /* 闹钟响铃计时 */
static uint8_t        g_dispDirty = 1;      /* 画面脏标记 */
static uint8_t        g_lastSec  = 0xFF;
static uint8_t        g_lastBlink = 0xFF;
static uint8_t        g_weekDayCache = 0;

/* 时间日期编辑状态 */
uint8_t g_timeEditMode  = 0;
uint8_t g_timeEditPos   = 0;   /* 0=年 1=月 2=日 3=星期 4=时 5=分 6=秒 */
uint8_t g_alarmEditMode = 0;
static RTC_TimeTypeDef g_editTime;
static RTC_DateTypeDef g_editDate;

QueueHandle_t xQueueKeyEvent = NULL;
QueueHandle_t xQueueSensorData = NULL;
QueueHandle_t xQueueHeartData = NULL;
static TaskHandle_t hDisp=NULL, hKey=NULL, hTime=NULL, hSens=NULL, hBle=NULL, hSw=NULL;

static const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

/* ==================== 辅助 ==================== */
static void RTC_Read(void)
{
    HAL_RTC_GetTime(&hrtc,&g_rtcTime,RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc,&g_rtcDate,RTC_FORMAT_BIN);
    if(g_weekDayCache) g_rtcDate.WeekDay = g_weekDayCache;
}

static void BuzzOn(void)  { HAL_GPIO_WritePin(BUZZER_GPIO_Port,BUZZER_Pin,GPIO_PIN_SET); HAL_GPIO_WritePin(GPIOB,GPIO_PIN_1,GPIO_PIN_SET); }
static void BuzzOff(void) { HAL_GPIO_WritePin(BUZZER_GPIO_Port,BUZZER_Pin,GPIO_PIN_RESET); HAL_GPIO_WritePin(GPIOB,GPIO_PIN_1,GPIO_PIN_RESET); }
static uint32_t g_beepEnd = 0;
static void BuzzBeep(void) { BuzzOn(); g_beepEnd = HAL_GetTick() + 50; }
static void SleepIn(void) { g_timeEditMode=0; g_alarmEditMode=0; OLED_DisplayOff(); g_sysMode.displayOn=0; }
static void SleepOut(void){ OLED_DisplayOn();  g_sysMode.displayOn=1; g_sysMode.lastKeyTick=HAL_GetTick(); }
static uint8_t blink(void){ return g_blink<50; }

static void RTC_WriteEdit(void)
{
    uint8_t savedWD = g_editDate.WeekDay;
    HAL_RTC_SetTime(&hrtc, &g_editTime, RTC_FORMAT_BIN);
    HAL_RTC_SetDate(&hrtc, &g_editDate, RTC_FORMAT_BIN);
    g_editDate.WeekDay = savedWD;
    g_weekDayCache = savedWD;
}

static void TimeEditEnter(void)
{
    g_editTime = g_rtcTime;
    g_editDate = g_rtcDate;
    g_timeEditMode = 1;
    g_timeEditPos = 4;
    g_dispDirty = 1;
}

static void TimeEditExit(void)
{
    RTC_WriteEdit();
    g_timeEditMode = 0;
    g_dispDirty = 1;
    RTC_Read();
}

/* ==================== 芯片深度休眠 ==================== */
static void EnterChipSleep(void)
{
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) return;
    if (HAL_GetTick() < 120000) return;
    EXTI->PR = (1 << 6) | (1 << 7) | (1 << 12) | (1 << 15);
    HAL_SuspendTick();
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
    SystemClock_Config();
    HAL_ResumeTick();
    MX_I2C1_Init();
    EXTI->PR = (1 << 6) | (1 << 7) | (1 << 12) | (1 << 15);
    SleepOut();
}

/* ==================== 通用界面框架 ==================== */
/**
  * @brief  绘制统一风格的顶部标题栏
  *   y=0: 实线  y=1~7: 标题文字  y=8: 实线
  */
static void DrawTopBar(const char *title, uint8_t hasBLE)
{
    OLED_DrawHLine(0, 0, 128, OLED_COLOR_WHITE);
    OLED_ShowString(4, 1, (uint8_t*)title, FONT_6X8);
    if (hasBLE && BLE_IsConnected())
        OLED_ShowString(108, 1, (uint8_t*)"BLE", FONT_6X8);
    OLED_DrawHLine(0, 9, 128, OLED_COLOR_WHITE);
}

/**
  * @brief  绘制统一风格的底部分隔线 + 提示文字
  *   y=H-8: 实线  y=H-7~H-1: 文字
  */
static void DrawBottomHint(const char *hint1, const char *hint2)
{
    OLED_DrawDashedHLine(0, 53, 128, OLED_COLOR_WHITE);
    if (hint1) OLED_ShowString(2, 55, (uint8_t*)hint1, FONT_6X8);
    if (hint2) OLED_ShowString(2, 62, (uint8_t*)hint2, FONT_6X8);
}

/* ==================== 界面0: 时间 ==================== */
static void DrawTime(void)
{
    char b[32];
    uint8_t y, m, d, hh, mm, ss, w;

    if(g_timeEditMode){
        y = g_editDate.Year;  m = g_editDate.Month;  d = g_editDate.Date;
        hh= g_editTime.Hours; mm= g_editTime.Minutes; ss= g_editTime.Seconds;
        w  = g_editDate.WeekDay;
    } else {
        y = g_rtcDate.Year;  m = g_rtcDate.Month;  d = g_rtcDate.Date;
        hh= g_rtcTime.Hours; mm= g_rtcTime.Minutes; ss= g_rtcTime.Seconds;
        w  = g_rtcDate.WeekDay;
    }

    OLED_ClearBuffer();

    if (g_timeEditMode)
    {
        /* ====== 编辑模式 — 保持当前页面布局, 闪烁编辑字段 ====== */
        /* 日期行 (和正常模式完全一样) */
        OLED_DrawHLine(0, 0, 128, OLED_COLOR_WHITE);
        snprintf(b, sizeof(b), "20%02d-%02d-%02d   %s", y, m, d, wd[w%7]);
        OLED_ShowString(4, 1, (uint8_t*)b, FONT_6X8);
        if (BLE_IsConnected())
            OLED_ShowString(108, 1, (uint8_t*)"BLE", FONT_6X8);
        OLED_DrawHLine(0, 9, 128, OLED_COLOR_WHITE);

        /* 大号时间 (和正常模式一样) */
        snprintf(b, sizeof(b), "%02d:%02d:%02d", hh, mm, ss);
        OLED_ShowLargeString(18, 16, (uint8_t*)b);

        /* 分隔线 */
        OLED_DrawDashedHLine(0, 43, 128, OLED_COLOR_WHITE);

        /* 编辑提示 */
        OLED_ShowString(2, 46, (uint8_t*)"FUNC:next", FONT_6X8);
        OLED_ShowString(62, 46, (uint8_t*)"UP/DN:chg", FONT_6X8);
        OLED_DrawHLine(0, 55, 128, OLED_COLOR_WHITE);
        OLED_ShowString(20, 57, (uint8_t*)"LONG-FUNC:save", FONT_6X8);

        /* 闪烁选中字段 (基于正常页面坐标) */
        if (blink()) {
            /* 日期行 y=1(6x8): 年[18,31] 月[39,52] 日[60,73] 星期[95,115]
               大号时间 y=16(12x24): 时[18~41] 分[52~75] 秒[86~109] */
            static const struct { uint8_t x; uint8_t y; uint8_t w; uint8_t h; } bp[7] = {
                {18, 1, 14, 8},    /* 0=年 */
                {39, 1, 14, 8},    /* 1=月 */
                {60, 1, 14, 8},    /* 2=日 */
                {95, 1, 21, 8},    /* 3=星期 */
                {18,16, 24,24},    /* 4=时 */
                {52,16, 24,24},    /* 5=分 */
                {86,16, 24,24},    /* 6=秒 */
            };
            uint8_t p = g_timeEditPos;
            if (p < 7) OLED_Fill(bp[p].x, bp[p].y,
                                 bp[p].x + bp[p].w,
                                 bp[p].y + bp[p].h - 1,
                                 OLED_COLOR_BLACK);
        }
    }
    else
    {
        /* ====== 正常时间显示 ====== */
        /* 顶部: 日期 */
        OLED_DrawHLine(0, 0, 128, OLED_COLOR_WHITE);
        snprintf(b, sizeof(b), "20%02d-%02d-%02d   %s", y, m, d, wd[w%7]);
        OLED_ShowString(4, 1, (uint8_t*)b, FONT_6X8);
        if (BLE_IsConnected())
            OLED_ShowString(108, 1, (uint8_t*)"BLE", FONT_6X8);
        OLED_DrawHLine(0, 9, 128, OLED_COLOR_WHITE);

        /* 中央: 大号时间 HH:MM:SS (6数字×12px+2冒号×10px=92px宽)
         * 水平居中: (128-92)/2=18, 垂直: y=16 → 跨度16~39 (24px=3页) */
        snprintf(b, sizeof(b), "%02d:%02d:%02d", hh, mm, ss);
        OLED_ShowLargeString(18, 16, (uint8_t*)b);

        /* 分隔线 */
        OLED_DrawDashedHLine(0, 43, 128, OLED_COLOR_WHITE);

        /* 底部: 温湿度 */
        if (g_envData.valid)
            snprintf(b, sizeof(b), "T:%.1fC  H:%.1f%%",
                     g_envData.temperature, g_envData.humidity);
        else
            snprintf(b, sizeof(b), "T:--.-C  H:--.-%%");
        OLED_ShowString(8, 46, (uint8_t*)b, FONT_6X8);

        /* 最底部提示 */
        OLED_DrawHLine(0, 55, 128, OLED_COLOR_WHITE);
        OLED_ShowString(12, 57, (uint8_t*)"LONG-FUNC:set time", FONT_6X8);
    }

    OLED_RefreshGram();
}

/* ==================== 界面1: 闹钟 ==================== */
static void DrawAlarm(void)
{
    char b[32];
    OLED_ClearBuffer();

    /* 顶部标题栏 */
    DrawTopBar("ALARM", 1);

    if (g_alarmEditMode)
    {
        /* ====== 闹钟编辑模式 ====== */
        /* 大号时间 (和时钟一样 12x24 字体) */
        snprintf(b, sizeof(b), "%02d:%02d:%02d",
                 g_alarm.hour, g_alarm.minute, g_alarm.second);
        OLED_ShowLargeString(18, 14, (uint8_t*)b);

        /* 闪烁编辑位: 时[18-41] 分[52-75] 秒[86-109] */
        if (blink()) {
            static const struct { uint8_t x; uint8_t y; uint8_t w; uint8_t h; } bp[3] = {
                {18,14, 24,24}, {52,14, 24,24}, {86,14, 24,24}
            };
            OLED_Fill(bp[g_setPos].x, bp[g_setPos].y,
                      bp[g_setPos].x+bp[g_setPos].w,
                      bp[g_setPos].y+bp[g_setPos].h-1, OLED_COLOR_BLACK);
        }

        /* "SET" 标签 */
        { uint8_t sx=(128-21)/2; OLED_ShowString(sx, 38, (uint8_t*)"SET", FONT_6X8); }

        /* 底部提示 */
        OLED_DrawDashedHLine(0, 48, 128, OLED_COLOR_WHITE);
        OLED_ShowString(8, 50, (uint8_t*)"FUNC:next", FONT_6X8);
        OLED_ShowString(70, 50, (uint8_t*)"UP/DN:chg", FONT_6X8);
        { uint8_t cx=(128-14*7)/2; OLED_ShowString(cx, 57, (uint8_t*)"LONG-FUNC:done", FONT_6X8); }
    }
    else
    {
        /* ====== 闹钟正常查看 ====== */
        /* 大号时间 (和时钟一样 12x24 字体居中) */
        snprintf(b, sizeof(b), "%02d:%02d:%02d",
                 g_alarm.hour, g_alarm.minute, g_alarm.second);
        OLED_ShowLargeString(18, 14, (uint8_t*)b);

        /* ON/OFF 紧贴框, 在时间下方居中 */
        {
            uint8_t fx, fw, tx;
            if (g_alarm.enable) {
                fw = 18;  /* "ON" 2字符×7 + 左右各2 */
                fx = (128 - fw) / 2;
                tx = fx + 2;
                OLED_DrawFrame(fx, 37, fw, 10, OLED_COLOR_WHITE);
                OLED_ShowString(tx, 38, (uint8_t*)"ON", FONT_6X8);
            } else {
                fw = 25;  /* "OFF" 3字符×7 + 左右各2 */
                fx = (128 - fw) / 2;
                tx = fx + 2;
                OLED_DrawFrame(fx, 37, fw, 10, OLED_COLOR_WHITE);
                OLED_ShowString(tx, 38, (uint8_t*)"OFF", FONT_6X8);
            }
        }

        /* 底部提示 */
        OLED_DrawDashedHLine(0, 48, 128, OLED_COLOR_WHITE);
        { uint8_t cx=(128-11*7)/2; OLED_ShowString(cx, 50, (uint8_t*)"FUNC:on/off", FONT_6X8); }
        { uint8_t cx=(128-14*7)/2; OLED_ShowString(cx, 57, (uint8_t*)"LONG-FUNC:edit", FONT_6X8); }
    }

    OLED_RefreshGram();
}

/* ==================== 界面2: 秒表 ==================== */
static void DrawStopwatch(void)
{
    char b[32];
    OLED_ClearBuffer();

    /* 顶部标题栏 */
    DrawTopBar("STOPWATCH", 1);

    /* 状态指示器 */
    const char *stateStr;
    switch (g_swState) {
        case STOPWATCH_RUNNING: stateStr = "RUN";  break;
        case STOPWATCH_STOPPED: stateStr = "STOP"; break;
        default:                stateStr = "RDY";  break;
    }
    OLED_ShowString(56, 12, (uint8_t*)stateStr, FONT_6X8);

    /* 秒表时间 (居中) */
    snprintf(b, sizeof(b), "%02d:%02d:%02d.%02d",
             g_swTime.hour, g_swTime.minute,
             g_swTime.second, g_swTime.centisecond);
    OLED_ShowString(20, 26, (uint8_t*)b, FONT_6X8);

    /* 操作提示 */
    OLED_DrawDashedHLine(0, 42, 128, OLED_COLOR_WHITE);
    if (g_swState == STOPWATCH_RUNNING)
        OLED_ShowString(28, 44, (uint8_t*)"FUNC:STOP", FONT_6X8);
    else
        OLED_ShowString(28, 44, (uint8_t*)"FUNC:START", FONT_6X8);

    OLED_DrawHLine(0, 53, 128, OLED_COLOR_WHITE);
    OLED_ShowString(12, 55, (uint8_t*)"LONG-FUNC:clear", FONT_6X8);

    OLED_RefreshGram();
}

/* ==================== 界面3: 心率 ==================== */
static void DrawHeart(void)
{
    char b[32];
    uint8_t x, len;
    OLED_ClearBuffer();

    /* 顶部标题栏 */
    DrawTopBar("HEART", 1);

    if (g_heartOn) {
        /* ====== 测量中 ====== */
        uint8_t hx, tw, bpmY = 22;  /* 大号数字下移避免与MEASURING重叠 */

        /* "MEASURING" 居中 */
        OLED_ShowString(32, 12, (uint8_t*)"MEASURING", FONT_6X8);

        /* "♥ 72 BPM" 大号数字居中, 心形和BPM分居两侧 */
        if (g_heartBPM > 0)
            snprintf(b, sizeof(b), "%d", g_heartBPM);
        else
            snprintf(b, sizeof(b), "--");
        len = strlen(b);
        /* 总宽: 心形8 + 间距2 + 数字len×12 + 间距2 + "BPM"3×7=21 */
        tw = 8 + 2 + len * 12 + 2 + 21;
        hx = (128 - tw) / 2;

        /* 左侧: 8×8 心形 (闪烁) */
        if (blink()) {
            uint8_t heart[8] = {0x00,0x6C,0xFE,0xFE,0x7C,0x38,0x10,0x00};
            uint8_t i, j;
            for (i = 0; i < 8; i++)
                for (j = 0; j < 8; j++)
                    OLED_DrawPoint(hx + j, bpmY + 8 + i,
                                   (heart[i] >> (7 - j)) & 1);
        }
        /* 中间: 大号BPM数字 */
        OLED_ShowLargeString(hx + 10, bpmY, (uint8_t*)b);
        /* 右侧: "BPM" 标签 */
        OLED_ShowString(hx + 10 + len * 12 + 2, bpmY + 8,
                        (uint8_t*)"BPM", FONT_6X8);

        /* 底部分隔 + 提示 */
        OLED_DrawDashedHLine(0, 46, 128, OLED_COLOR_WHITE);
        OLED_ShowString(32, 48, (uint8_t*)"FUNC:STOP", FONT_6X8);
    } else {
        /* ====== 待机 ====== */
        /* "READY" 居中: 5字符×7px=35px → (128-35)/2=46 */
        OLED_ShowString(46, 12, (uint8_t*)"READY", FONT_6X8);

        /* 8×8 心形图标居中: (128-8)/2=60 */
        {
            uint8_t heart[8] = {0x00,0x6C,0xFE,0xFE,0x7C,0x38,0x10,0x00};
            uint8_t i, j;
            for (i = 0; i < 8; i++) {
                for (j = 0; j < 8; j++) {
                    OLED_DrawPoint(60 + j, 20 + i,
                                   (heart[i] >> (7 - j)) & 1);
                }
            }
        }

        /* 上次测量值 居中 */
        if (g_heartBPM > 0) {
            snprintf(b, sizeof(b), "Last: %d BPM", g_heartBPM);
        } else {
            snprintf(b, sizeof(b), "No Data");
        }
        /* 动态居中: 字符数×7px */
        len = strlen(b);
        x = (128 - len * 7) / 2;
        OLED_ShowString(x, 32, (uint8_t*)b, FONT_6X8);

        /* 底部提示 */
        OLED_DrawDashedHLine(0, 46, 128, OLED_COLOR_WHITE);
        /* "FUNC:START" 居中: 10字符×7px=70px → (128-70)/2=29 */
        OLED_ShowString(29, 48, (uint8_t*)"FUNC:START", FONT_6X8);
        /* 测量范围提示 */
        OLED_ShowString(11, 55, (uint8_t*)"Range:40-180BPM", FONT_6X8);
    }

    OLED_RefreshGram();
}

/* ==================== 显示任务 ==================== */
void vTask_Display(void *pvParameters)
{
    TickType_t t=xTaskGetTickCount();
    for(;;){
        ahtData_t env;
        uint8_t bpm;
        /* receive latest env data from sensor queue (non-blocking) */
        if(xQueueReceive(xQueueSensorData, &env, 0) == pdTRUE){
            g_envData = env;
        }
        if(xQueueReceive(xQueueHeartData, &bpm, 0) == pdTRUE){
            g_heartBPM = bpm;
        }
        if(g_sysMode.displayOn){
            uint8_t s  = g_rtcTime.Seconds;
            uint8_t bl = blink();
            /* 心率/秒表页面: 强制持续刷新 */
            if(g_sysMode.currentScreen == SCREEN_HEART ||
               g_sysMode.currentScreen == SCREEN_STOPWATCH)
            {
                g_lastSec=s; g_lastBlink=bl; g_dispDirty=0;
                if(g_sysMode.currentScreen == SCREEN_HEART)
                    DrawHeart();
                else
                    DrawStopwatch();
            }
            /* 其他页面: 按需刷新 */
            else if(s!=g_lastSec || bl!=g_lastBlink || g_dispDirty)
            {
                g_lastSec=s; g_lastBlink=bl; g_dispDirty=0;
                switch(g_sysMode.currentScreen){
                    case SCREEN_TIME:  DrawTime();  break;
                    case SCREEN_ALARM: DrawAlarm(); break;
                    default: break;
                }
            }
        }
        vTaskDelayUntil(&t,pdMS_TO_TICKS(100));
    }
}

/* ==================== 按键任务 ==================== */
void vTask_KeyScan(void *pvParameters)
{
    TickType_t t=xTaskGetTickCount();
    KeyEvent_t e;
    KEY_Init();
    for(;;){
        /* === 蓝牙远程命令 === */
        {
            uint8_t bc = BLE_GetCmd();
            if (bc != BLE_CMD_NONE)
            {
                if (!g_sysMode.displayOn) { SleepOut(); }
                g_sysMode.lastKeyTick = HAL_GetTick();
                BuzzBeep();
                switch (bc)
                {
                    case BLE_CMD_MODE:
                        g_sysMode.currentScreen = (g_sysMode.currentScreen + 1) % SCREEN_MAX;
                        if (g_sysMode.currentScreen == SCREEN_STOPWATCH)
                            { g_swState = STOPWATCH_IDLE; memset(&g_swTime,0,sizeof(g_swTime)); }
                        break;
                    case BLE_CMD_FUNC:
                        if(g_sysMode.currentScreen==SCREEN_TIME && g_timeEditMode)
                            g_timeEditPos=(g_timeEditPos+1)%7;
                        else if(g_sysMode.currentScreen==SCREEN_ALARM && g_alarmEditMode)
                            g_setPos=(g_setPos+1)%3;
                        else if(g_sysMode.currentScreen==SCREEN_ALARM)
                            g_alarm.enable=!g_alarm.enable;
                        else if(g_sysMode.currentScreen==SCREEN_STOPWATCH){
                            if(g_swState==STOPWATCH_RUNNING) g_swState=STOPWATCH_STOPPED;
                            else g_swState=STOPWATCH_RUNNING;
                        }
                        else if(g_sysMode.currentScreen==SCREEN_HEART)
                            {if(g_heartOn){g_heartOn=0;HEART_StopMeasure();}else{g_heartOn=1;HEART_StartMeasure();}}
                        break;
                    case BLE_CMD_UP:
                        if(g_sysMode.currentScreen==SCREEN_TIME && g_timeEditMode){
                            switch(g_timeEditPos){
                                case 0: g_editDate.Year=(g_editDate.Year+1)%100; break;
                                case 1: g_editDate.Month=(g_editDate.Month%12)+1; break;
                                case 2: g_editDate.Date=(g_editDate.Date%31)+1; break;
                                case 4: g_editTime.Hours=(g_editTime.Hours+1)%24; break;
                                case 5: g_editTime.Minutes=(g_editTime.Minutes+1)%60; break;
                                case 6: g_editTime.Seconds=(g_editTime.Seconds+1)%60; break;
                                case 3: g_editDate.WeekDay=g_editDate.WeekDay%7+1; break;
                            }
                            RTC_WriteEdit();
                        }
                        else if(g_sysMode.currentScreen==SCREEN_ALARM && g_alarmEditMode){
                            if(g_setPos==0)g_alarm.hour=(g_alarm.hour+1)%24;
                            else if(g_setPos==1)g_alarm.minute=(g_alarm.minute+1)%60;
                            else g_alarm.second=(g_alarm.second+1)%60;
                        }
                        break;
                    case BLE_CMD_DOWN:
                        if(g_sysMode.currentScreen==SCREEN_TIME && g_timeEditMode){
                            switch(g_timeEditPos){
                                case 0: g_editDate.Year=(g_editDate.Year+99)%100; break;
                                case 1: g_editDate.Month=((g_editDate.Month+10)%12)+1; break;
                                case 2: g_editDate.Date=((g_editDate.Date+29)%31)+1; break;
                                case 4: g_editTime.Hours=(g_editTime.Hours+23)%24; break;
                                case 5: g_editTime.Minutes=(g_editTime.Minutes+59)%60; break;
                                case 6: g_editTime.Seconds=(g_editTime.Seconds+59)%60; break;
                                case 3: g_editDate.WeekDay=(g_editDate.WeekDay+5)%7+1; break;
                            }
                            RTC_WriteEdit();
                        }
                        else if(g_sysMode.currentScreen==SCREEN_ALARM && g_alarmEditMode){
                            if(g_setPos==0)g_alarm.hour=(g_alarm.hour+23)%24;
                            else if(g_setPos==1)g_alarm.minute=(g_alarm.minute+59)%60;
                            else g_alarm.second=(g_alarm.second+59)%60;
                        }
                        break;
                    case BLE_CMD_FUNC_LONG:
                        if (g_sysMode.currentScreen == SCREEN_TIME) {
                            if(g_timeEditMode) TimeEditExit();
                            else TimeEditEnter();
                        }
                        if (g_sysMode.currentScreen == SCREEN_ALARM) {
                            if(g_alarmEditMode) g_alarmEditMode=0;
                            else {g_alarmEditMode=1;g_setPos=0;}
                        }
                        if (g_sysMode.currentScreen == SCREEN_STOPWATCH)
                            {g_swState=STOPWATCH_IDLE;memset(&g_swTime,0,sizeof(g_swTime));}
                        break;
                }
                goto tick;
            }
        }

        e=KEY_GetEvent();
        if(e.event!=KEY_EVENT_NONE){
            if(!g_sysMode.displayOn){SleepOut();}
            g_sysMode.lastKeyTick=HAL_GetTick();
            g_dispDirty=1;
            BuzzBeep();
            if(g_ringTick){BuzzOff();g_ringTick=0;goto tick;}

            switch(g_sysMode.currentScreen){
            case SCREEN_TIME:
                if(g_timeEditMode){
                    if(e.keyNum==KEY_MODE&&e.event==KEY_EVENT_SHORT)
                        {TimeEditExit();g_sysMode.currentScreen=SCREEN_ALARM;}
                    else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_SHORT){
                        g_timeEditPos=(g_timeEditPos+1)%7;
                        g_dispDirty=1;
                    }
                    else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_LONG)
                        TimeEditExit();
                    else if(e.keyNum==KEY_UP&&e.event==KEY_EVENT_SHORT){
                        switch(g_timeEditPos){
                            case 0: g_editDate.Year=(g_editDate.Year+1)%100; break;
                            case 1: g_editDate.Month=(g_editDate.Month%12)+1; break;
                            case 2: g_editDate.Date=(g_editDate.Date%31)+1; break;
                            case 4: g_editTime.Hours=(g_editTime.Hours+1)%24; break;
                            case 5: g_editTime.Minutes=(g_editTime.Minutes+1)%60; break;
                            case 6: g_editTime.Seconds=(g_editTime.Seconds+1)%60; break;
                            case 3: g_editDate.WeekDay=g_editDate.WeekDay%7+1; break;
                        }
                        RTC_WriteEdit();
                        g_dispDirty=1;
                    }
                    else if(e.keyNum==KEY_DOWN&&e.event==KEY_EVENT_SHORT){
                        switch(g_timeEditPos){
                            case 0: g_editDate.Year=(g_editDate.Year+99)%100; break;
                            case 1: g_editDate.Month=((g_editDate.Month+10)%12)+1; break;
                            case 2: g_editDate.Date=((g_editDate.Date+29)%31)+1; break;
                            case 4: g_editTime.Hours=(g_editTime.Hours+23)%24; break;
                            case 5: g_editTime.Minutes=(g_editTime.Minutes+59)%60; break;
                            case 6: g_editTime.Seconds=(g_editTime.Seconds+59)%60; break;
                            case 3: g_editDate.WeekDay=(g_editDate.WeekDay+5)%7+1; break;
                        }
                        RTC_WriteEdit();
                        g_dispDirty=1;
                    }
                } else {
                    if(e.keyNum==KEY_MODE&&e.event==KEY_EVENT_SHORT)
                        g_sysMode.currentScreen=SCREEN_ALARM;
                    else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_LONG)
                        TimeEditEnter();
                }
                break;
            case SCREEN_ALARM:
                if(e.keyNum==KEY_MODE&&e.event==KEY_EVENT_SHORT){
                    g_alarmEditMode=0;
                    g_sysMode.currentScreen=SCREEN_STOPWATCH;
                    g_swState=STOPWATCH_IDLE;memset(&g_swTime,0,sizeof(g_swTime));
                }
                if(g_alarmEditMode){
                    if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_SHORT){
                        g_setPos=(g_setPos+1)%3;
                        g_dispDirty=1;
                    }
                    else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_LONG){
                        g_alarmEditMode=0;
                        g_dispDirty=1;
                    }
                    else if(e.keyNum==KEY_UP&&e.event==KEY_EVENT_SHORT){
                        if(g_setPos==0)g_alarm.hour=(g_alarm.hour+1)%24;
                        else if(g_setPos==1)g_alarm.minute=(g_alarm.minute+1)%60;
                        else g_alarm.second=(g_alarm.second+1)%60;
                        g_dispDirty=1;
                    }
                    else if(e.keyNum==KEY_DOWN&&e.event==KEY_EVENT_SHORT){
                        if(g_setPos==0)g_alarm.hour=(g_alarm.hour+23)%24;
                        else if(g_setPos==1)g_alarm.minute=(g_alarm.minute+59)%60;
                        else g_alarm.second=(g_alarm.second+59)%60;
                        g_dispDirty=1;
                    }
                } else {
                    if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_SHORT){
                        g_alarm.enable=!g_alarm.enable;
                        g_dispDirty=1;
                    }
                    else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_LONG){
                        g_alarmEditMode=1;
                        g_setPos=0;
                        g_dispDirty=1;
                    }
                }
                break;
            case SCREEN_STOPWATCH:
                if(e.keyNum==KEY_MODE&&e.event==KEY_EVENT_SHORT)
                    {g_sysMode.currentScreen=SCREEN_HEART;}
                else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_SHORT){
                    if(g_swState==STOPWATCH_RUNNING)
                        g_swState=STOPWATCH_STOPPED;
                    else
                        g_swState=STOPWATCH_RUNNING;
                }
                else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_LONG){
                    g_swState=STOPWATCH_IDLE;
                    memset(&g_swTime,0,sizeof(g_swTime));
                }
                break;
            case SCREEN_HEART:
                if(e.keyNum==KEY_MODE&&e.event==KEY_EVENT_SHORT)
                    {g_heartOn=0;HEART_StopMeasure();g_sysMode.currentScreen=SCREEN_TIME;}
                else if(e.keyNum==KEY_FUNC&&e.event==KEY_EVENT_SHORT){
                    if(g_heartOn){g_heartOn=0;HEART_StopMeasure();}
                    else{g_heartOn=1;HEART_StartMeasure();}
                }
                break;
            default: break;
            }
        }
tick:
        if(g_beepEnd && HAL_GetTick() >= g_beepEnd) { BuzzOff(); g_beepEnd = 0; }
        g_blink=(g_blink+1)%100;
        vTaskDelayUntil(&t,pdMS_TO_TICKS(10));
    }
}

/* ==================== 时间任务 ==================== */
void vTask_Time(void *pvParameters)
{
    TickType_t t=xTaskGetTickCount();
    for(;;){
        RTC_Read();
        if(g_alarm.enable&&!g_ringTick&&g_rtcTime.Hours==g_alarm.hour&&g_rtcTime.Minutes==g_alarm.minute&&g_rtcTime.Seconds==g_alarm.second)
        {g_ringTick=HAL_GetTick();BuzzOn();}
        if(g_ringTick&&HAL_GetTick()-g_ringTick>3000){BuzzOff();g_ringTick=0;}
        g_sysMode.systemTick++;
        vTaskDelayUntil(&t,pdMS_TO_TICKS(1000));
    }
}

/* ==================== 传感器任务 ==================== */
void vTask_Sensor(void *pvParameters)
{
    TickType_t t=xTaskGetTickCount();
    uint32_t cyc=0; uint16_t adc; ahtData_t env; uint8_t bpm;
    AHT20_RTOS_Init(); AHT20_Init(); HEART_Init();

    /* 上电即启动ADC连续转换 (此后一直运行, 不启停) */
    ADC1->CR2 |= ADC_CR2_ADON;    /* ADC上电 */
    osDelay(2);                  /* 等待稳定 */
    ADC1->CR2 |= ADC_CR2_SWSTART;  /* 启动连续转换 */

    for(;;){
        /* 连续模式下直接读DR (无需等EOC, ADC一直在转) */
        adc = ADC1->DR;
        HEART_ProcessSample(adc);
        if(g_heartOn){
            bpm = HEART_GetBPM();
            xQueueOverwrite(xQueueHeartData, &bpm); /* send bpm via FreeRTOS queue */
        }
        cyc++;
        if(cyc>=100){
            cyc=0;
            if(AHT20_ReadData(&env)){
                xQueueSend(xQueueSensorData, &env, 0); /* send env via FreeRTOS queue */
            }
        }
        vTaskDelayUntil(&t,pdMS_TO_TICKS(20));
    }
}

/* ==================== 蓝牙任务 ==================== */
void vTask_Bluetooth(void *pvParameters)
{
    TickType_t t=xTaskGetTickCount();
    uint32_t last=0; char b[64];
    uint8_t ch;
    static char rspBuf[128];  /* 静态缓冲区, 不占任务栈 */
    uint8_t needReport = 0;
    BLE_Init();

    for(;;){
        while (BLE_PollRx(&ch))
        {
            g_sysMode.lastKeyTick = HAL_GetTick();
            if (ch=='A'||ch=='a'||ch=='B'||ch=='b'||
                ch=='C'||ch=='c'||ch=='D'||ch=='d'||
                ch=='E'||ch=='e')
            {
                if (!g_sysMode.displayOn) { OLED_DisplayOn(); g_sysMode.displayOn=1; }
                g_dispDirty=1;
                BuzzBeep();

                if (ch=='A'||ch=='a')
                    { g_sysMode.currentScreen=(g_sysMode.currentScreen+1)%SCREEN_MAX;
                      if(g_sysMode.currentScreen==SCREEN_STOPWATCH)
                          {g_swState=STOPWATCH_IDLE;memset(&g_swTime,0,sizeof(g_swTime));} }
                else if (ch=='B'||ch=='b')
                {
                    if(g_sysMode.currentScreen==SCREEN_TIME && g_timeEditMode)
                        g_timeEditPos=(g_timeEditPos+1)%7;
                    else if(g_sysMode.currentScreen==SCREEN_ALARM && g_alarmEditMode)
                        g_setPos=(g_setPos+1)%3;
                    else if(g_sysMode.currentScreen==SCREEN_ALARM)
                        g_alarm.enable=!g_alarm.enable;
                    else if(g_sysMode.currentScreen==SCREEN_STOPWATCH){
                        if(g_swState==STOPWATCH_RUNNING) g_swState=STOPWATCH_STOPPED;
                        else g_swState=STOPWATCH_RUNNING;
                    }
                    else if(g_sysMode.currentScreen==SCREEN_HEART)
                        {if(g_heartOn){g_heartOn=0;HEART_StopMeasure();}else{g_heartOn=1;HEART_StartMeasure();}}
                }
                else if (ch=='C'||ch=='c')
                {
                    if(g_sysMode.currentScreen==SCREEN_TIME && g_timeEditMode){
                        switch(g_timeEditPos){
                            case 0: g_editDate.Year=(g_editDate.Year+1)%100; break;
                            case 1: g_editDate.Month=(g_editDate.Month%12)+1; break;
                            case 2: g_editDate.Date=(g_editDate.Date%31)+1; break;
                            case 4: g_editTime.Hours=(g_editTime.Hours+1)%24; break;
                            case 5: g_editTime.Minutes=(g_editTime.Minutes+1)%60; break;
                            case 6: g_editTime.Seconds=(g_editTime.Seconds+1)%60; break;
                            case 3: g_editDate.WeekDay=g_editDate.WeekDay%7+1; break;
                        }
                        RTC_WriteEdit();
                    }
                    else if(g_sysMode.currentScreen==SCREEN_ALARM && g_alarmEditMode){
                        if(g_setPos==0)g_alarm.hour=(g_alarm.hour+1)%24;
                        else if(g_setPos==1)g_alarm.minute=(g_alarm.minute+1)%60;
                        else g_alarm.second=(g_alarm.second+1)%60;
                    }
                }
                else if (ch=='D'||ch=='d')
                {
                    if(g_sysMode.currentScreen==SCREEN_TIME && g_timeEditMode){
                        switch(g_timeEditPos){
                            case 0: g_editDate.Year=(g_editDate.Year+99)%100; break;
                            case 1: g_editDate.Month=((g_editDate.Month+10)%12)+1; break;
                            case 2: g_editDate.Date=((g_editDate.Date+29)%31)+1; break;
                            case 4: g_editTime.Hours=(g_editTime.Hours+23)%24; break;
                            case 5: g_editTime.Minutes=(g_editTime.Minutes+59)%60; break;
                            case 6: g_editTime.Seconds=(g_editTime.Seconds+59)%60; break;
                            case 3: g_editDate.WeekDay=(g_editDate.WeekDay+5)%7+1; break;
                        }
                        RTC_WriteEdit();
                    }
                    else if(g_sysMode.currentScreen==SCREEN_ALARM && g_alarmEditMode){
                        if(g_setPos==0)g_alarm.hour=(g_alarm.hour+23)%24;
                        else if(g_setPos==1)g_alarm.minute=(g_alarm.minute+59)%60;
                        else g_alarm.second=(g_alarm.second+59)%60;
                    }
                }
                else
                {
                    if(g_sysMode.currentScreen==SCREEN_TIME)
                    {
                        if(g_timeEditMode) TimeEditExit();
                        else TimeEditEnter();
                    }
                    else if(g_sysMode.currentScreen==SCREEN_ALARM)
                    {
                        if(g_alarmEditMode) g_alarmEditMode=0;
                        else {g_alarmEditMode=1;g_setPos=0;}
                    }
                    else if(g_sysMode.currentScreen==SCREEN_STOPWATCH)
                        {g_swState=STOPWATCH_IDLE;memset(&g_swTime,0,sizeof(g_swTime));}
                }
            }
            else if (ch == 'R' || ch == 'r')
            {
                needReport = 1;  /* 标记, 在循环外发送 */
            }
        }

        /* 在轮询循环外发送响应, 避免UART收发冲突 */
        if (needReport)
        {
            needReport = 0;
            /* 直接读RTC, 使用vTask_Time缓存的变量避免竞态 */
            {
                const char *swStateStr;
                switch (g_swState) {
                    case STOPWATCH_RUNNING: swStateStr = "RUN";  break;
                    case STOPWATCH_STOPPED: swStateStr = "STOP"; break;
                    default:               swStateStr = "IDLE"; break;
                }
                snprintf(rspBuf, sizeof(rspBuf),
                    "T:%02d-%02d-%02d %02d:%02d:%02d\r\n"
                    "A:%02d:%02d:%02d %s\r\n"
                    "E:%.1fC %.1f%%\r\n"
                    "H:%d\r\n"
                    "S:%02d:%02d:%02d.%02d %s\r\n",
                    g_rtcDate.Year, g_rtcDate.Month, g_rtcDate.Date,
                    g_rtcTime.Hours, g_rtcTime.Minutes, g_rtcTime.Seconds,
                    g_alarm.hour, g_alarm.minute, g_alarm.second,
                    g_alarm.enable ? "ON" : "OFF",
                    g_envData.valid ? g_envData.temperature : 0.0f,
                    g_envData.valid ? g_envData.humidity : 0.0f,
                    g_heartBPM,
                    g_swTime.hour, g_swTime.minute,
                    g_swTime.second, g_swTime.centisecond,
                    swStateStr);
            }
            HAL_UART_Transmit(&huart1, (uint8_t *)rspBuf, strlen(rspBuf), 200);
        }

        if(BLE_IsConnected()&&HAL_GetTick()-last>5000){
            last=HAL_GetTick();
            if(g_heartBPM>0){snprintf(b,sizeof(b),"%d",g_heartBPM);BLE_SendData(3,b);}
        }
        vTaskDelayUntil(&t,pdMS_TO_TICKS(10));
    }
}

/* ==================== 秒表任务 ==================== */
void vTask_Stopwatch(void *pvParameters)
{
    TickType_t t=xTaskGetTickCount();
    for(;;){
        if(g_swState==STOPWATCH_RUNNING){
            g_swTime.centisecond++;
            if(g_swTime.centisecond>=100){g_swTime.centisecond=0;g_swTime.second++;}
            if(g_swTime.second>=60){g_swTime.second=0;g_swTime.minute++;}
            if(g_swTime.minute>=60){g_swTime.minute=0;g_swTime.hour++;}
            if(g_swTime.hour>=1){g_swTime.hour=1;g_swTime.minute=0;g_swTime.second=0;g_swTime.centisecond=0;g_swState=STOPWATCH_STOPPED;}
        }
        vTaskDelayUntil(&t,pdMS_TO_TICKS(10));
    }
}

/* ==================== 启动 ==================== */
void appStart(void)
{
    memset(&g_sysMode,0,sizeof(g_sysMode));
    memset(&g_swTime,0,sizeof(g_swTime));
    g_sysMode.currentScreen=SCREEN_TIME;
    g_sysMode.displayOn=1;
    g_sysMode.lastKeyTick=HAL_GetTick();
    RTC_Read();
    KEY_EXTI_WakeupInit();
    xQueueKeyEvent=xQueueCreate(4,sizeof(KeyEvent_t));
    xQueueSensorData=xQueueCreate(4,sizeof(ahtData_t));
    xQueueHeartData=xQueueCreate(1,sizeof(uint8_t));
    xTaskCreate(vTask_Stopwatch,"Sw", 64, NULL,4,&hSw);
    xTaskCreate(vTask_KeyScan,  "Key",128,NULL,3,&hKey);
    xTaskCreate(vTask_Bluetooth,"BLE",128,NULL,3,&hBle);
    xTaskCreate(vTask_Display,  "Dsp",256,NULL,2,&hDisp);
    xTaskCreate(vTask_Time,     "Tim",64, NULL,2,&hTime);
    xTaskCreate(vTask_Sensor,   "Sen",200,NULL,2,&hSens);  /* prio=2 和显示同级 */
}
