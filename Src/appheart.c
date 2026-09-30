/**
  ******************************************************************************
  * @file           : appheart.c
  * @brief          : 心率测量实现 (v3 — 高灵敏度版)
  * @note           : 结合原始算法持续振幅跟踪 + 真峰值检测 + 中值滤波:
  *                   1. 持续跟踪 acMax/acMin (指数衰减), 阈值实时跟随信号变化
  *                   2. 真峰值状态机 — 追踪最大值的时刻 (精确IBI计时)
  *                   3. IBI中值滤波 (5样本) — 抗离群值
  *                   4. 自适应不应期 — IBI中值×40%
  *                   5. 高增益×8 + 低偏移 — 适配弱信号
  *                   6. 双时间常数DC — 快收敛+慢稳态
  ******************************************************************************
  */
#include "appheart.h"
#include "adc.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

static uint8_t  heartState = HEART_STATE_IDLE;
static uint8_t  heartBPM = 0;
static uint16_t lastADC = 0;
static uint16_t sampleBuf[HEART_SAMPLE_BUF_SIZE];
static uint16_t sampleIdx = 0;

/* ==================== 信号参数 ==================== */
#define AC_GAIN         8          /* 固定增益 (高增益适配弱信号) */
#define AC_DECAY_NUM    7          /* 振幅衰减分子 (7/8 = 0.875) */
#define AC_DECAY_DEN    8          /* 振幅衰减分母 */
#define THRESH_OFFSET   12         /* 阈值偏移 (抬高以过滤噪声假触发) */

/* ==================== 真峰值检测状态机 ==================== */
typedef enum {
    PEAK_IDLE = 0,
    PEAK_RISING,
} PeakState_t;

static PeakState_t peakState = PEAK_IDLE;
static uint32_t lastBeatTick = 0;
static uint32_t peakTick = 0;
static int16_t  peakMax = 0;       /* 当前脉冲追踪到的最大值 */

/* ==================== 持续振幅跟踪 (原算法优势) ==================== */
static int16_t acMax = 0;          /* 交流分量最大值 (持续跟踪) */
static int16_t acMin = 0;          /* 交流分量最小值 (持续跟踪) */

/* ==================== IBI中值滤波 ==================== */
#define IBI_BUF_SIZE    8          /* 增大窗口 → 中值更稳定 */
static uint16_t ibiBuf[IBI_BUF_SIZE];
static uint8_t  ibiIdx = 0;
static uint8_t  ibiCnt = 0;

/* ==================== BPM输出平滑 ==================== */
#define BPM_HIST_SIZE   8          /* 增大窗口 → 输出更平滑 */
static uint16_t bpmHist[BPM_HIST_SIZE];
static uint8_t  bpmHistIdx = 0;
static uint8_t  bpmHistCnt = 0;    /* 实际已写入的样本数 */

/* ==================== DC跟踪 ==================== */
static uint16_t dcFast = 2048;     /* 快速 α=1/8 */
static uint16_t dcSlow = 2048;     /* 慢速 α=1/64 */
static uint16_t dcLevel = 2048;
static uint32_t dcSampleCnt = 0;

/* ==================== 不应期 ==================== */
static uint16_t refracMs = 250;    /* 当前不应期 (动态调整) */


/* ==================== 辅助函数 ==================== */
static void insertSort(uint16_t *arr, uint8_t n)
{
    uint8_t i, j;
    uint16_t key;
    for (i = 1; i < n; i++) {
        key = arr[i];
        for (j = i; j > 0 && arr[j-1] > key; j--)
            arr[j] = arr[j-1];
        arr[j] = key;
    }
}

static uint16_t median5(uint16_t *arr, uint8_t n)
{
    uint16_t tmp[IBI_BUF_SIZE];
    memcpy(tmp, arr, n * sizeof(uint16_t));
    insertSort(tmp, n);
    return tmp[n / 2];
}


/* ==================== 初始化 ==================== */
void HEART_Init(void)
{
    heartState = HEART_STATE_IDLE;
    heartBPM = 0;
    sampleIdx = 0;
    peakState = PEAK_IDLE;
    lastBeatTick = 0;
    peakTick = 0;
    peakMax = 0;
    acMax = 0;
    acMin = 0;
    ibiIdx = 0;
    ibiCnt = 0;
    bpmHistIdx = 0;
    bpmHistCnt = 0;
    dcFast = 2048;
    dcSlow = 2048;
    dcLevel = 2048;
    dcSampleCnt = 0;
    refracMs = 250;
    memset(sampleBuf, 0, sizeof(sampleBuf));
    memset(ibiBuf, 0, sizeof(ibiBuf));
    memset(bpmHist, 0, sizeof(bpmHist));
}

void HEART_StartMeasure(void)
{
    heartState = HEART_STATE_MEASURING;
    heartBPM = 0;
    peakState = PEAK_IDLE;
    lastBeatTick = 0;
    peakTick = 0;
    peakMax = 0;
    acMax = 0;
    acMin = 0;
    ibiIdx = 0;
    ibiCnt = 0;
    bpmHistIdx = 0;
    bpmHistCnt = 0;
    dcFast = dcLevel;
    dcSampleCnt = 0;
    refracMs = 250;
    memset(sampleBuf, 0, sizeof(sampleBuf));
    memset(ibiBuf, 0, sizeof(ibiBuf));
    memset(bpmHist, 0, sizeof(bpmHist));
}

void HEART_StopMeasure(void)        { heartState = HEART_STATE_IDLE; }
uint8_t HEART_GetBPM(void)          { return heartBPM; }
uint16_t HEART_GetLastADC(void)     { return lastADC; }
uint8_t HEART_GetState(void)        { return heartState; }


/* ==================== BPM计算 (中值+滑动平均+变化率限制) ==================== */
static void HEART_CalculateBPM(void)
{
    uint16_t medianIBI, rawBPM, smoothedBPM;
    uint32_t sum;
    uint8_t i, n;
    int16_t delta;

    if (ibiCnt < 2) return;

    n = ibiCnt < IBI_BUF_SIZE ? ibiCnt : IBI_BUF_SIZE;
    medianIBI = median5(ibiBuf, n);

    if (medianIBI < 300 || medianIBI > 1500) return;  /* 40~200 BPM */
    rawBPM = 60000 / medianIBI;

    if (rawBPM < 40)  rawBPM = 40;
    if (rawBPM > 200) rawBPM = 200;

    /* 滑动平均 (只用实际已写入的样本数) */
    bpmHist[bpmHistIdx % BPM_HIST_SIZE] = rawBPM;
    bpmHistIdx++;
    if (bpmHistCnt < BPM_HIST_SIZE) bpmHistCnt++;
    sum = 0;
    for (i = 0; i < bpmHistCnt; i++) sum += bpmHist[i];
    smoothedBPM = (uint16_t)(sum / bpmHistCnt);

    /* 变化率限制: 每次最多±2 BPM, 防止跳变 */
    if (heartBPM > 0) {
        delta = (int16_t)smoothedBPM - (int16_t)heartBPM;
        if (delta > 2)       heartBPM = heartBPM + 2;
        else if (delta < -2) heartBPM = heartBPM - 2;
        else                 heartBPM = (uint8_t)smoothedBPM;
    } else {
        /* 首次出数直接赋值 */
        heartBPM = (uint8_t)smoothedBPM;
    }
}


/* ==================== 记录IBI ==================== */
static void RecordIBI(uint16_t ibi)
{
    uint16_t medianIBI_prev;
    uint8_t n;

    /* 更新自适应不应期 */
    if (ibiCnt >= 2) {
        n = ibiCnt < IBI_BUF_SIZE ? ibiCnt : IBI_BUF_SIZE;
        medianIBI_prev = median5(ibiBuf, n);
        refracMs = medianIBI_prev * 35 / 100;
        if (refracMs < 250)  refracMs = 250;
        if (refracMs > 1000) refracMs = 1000;
    }

    /* 基本范围: 300ms(200BPM) ~ 1500ms(40BPM) */
    if (ibi < 300 || ibi > 1500) return;

    /* 一致性: 偏离中值>40%丢弃 (更严格, 减少跳变) */
    if (ibiCnt >= 2) {
        uint32_t diff = (ibi > medianIBI_prev) ? (ibi - medianIBI_prev) : (medianIBI_prev - ibi);
        if (diff > medianIBI_prev * 40 / 100) return;
    }

    ibiBuf[ibiIdx % IBI_BUF_SIZE] = ibi;
    ibiIdx++;
    if (ibiCnt < IBI_BUF_SIZE) ibiCnt++;

    HEART_CalculateBPM();
}


/* ==================== 主处理函数 ==================== */
void HEART_ProcessSample(uint16_t adcValue)
{
    uint32_t now;
    int16_t ac;
    int16_t thr;

    lastADC = adcValue;
    if (heartState != HEART_STATE_MEASURING) return;

    now = xTaskGetTickCount();
    dcSampleCnt++;

    /* ====== 1. DC直流跟踪 ====== */
    dcFast = (dcFast * 7 + adcValue) / 8;
    dcSlow = (dcSlow * 63 + adcValue) / 64;

    if (dcSampleCnt < 100) {
        dcLevel = dcFast;
    } else {
        uint32_t w = (dcSampleCnt > 200) ? 64 : (dcSampleCnt - 100) * 64 / 100;
        dcLevel = (uint16_t)((dcSlow * w + dcFast * (64 - w)) / 64);
    }

    /* ====== 2. 提取AC + 高增益 ====== */
    ac = (int16_t)adcValue - (int16_t)dcLevel;
    ac = ac * AC_GAIN;

    /* ====== 3. 持续跟踪振幅 (指数衰减, 实时适应信号变化) ====== */
    if (ac > acMax) acMax = ac;
    if (ac < acMin) acMin = ac;

    /* 每采样衰减, 保持振幅跟踪时效性 */
    acMax = acMax * AC_DECAY_NUM / AC_DECAY_DEN;
    acMin = acMin * AC_DECAY_NUM / AC_DECAY_DEN;

    /* ====== 4. 动态阈值 = 振幅中点 + 偏移 ====== */
    thr = (acMax + acMin) / 2 + THRESH_OFFSET;
    if (thr < THRESH_OFFSET * 3) thr = THRESH_OFFSET * 3;  /* 最小=36 */

    /* ====== 5. 保存到调试缓冲区 ====== */
    sampleBuf[sampleIdx % HEART_SAMPLE_BUF_SIZE] = (uint16_t)(2048 + ac);
    sampleIdx++;

    /* ====== 6. 真峰值检测状态机 ====== */
    switch (peakState) {

    case PEAK_IDLE:
        if (ac > thr) {
            peakState = PEAK_RISING;
            peakMax = ac;
            peakTick = now;
        }
        break;

    case PEAK_RISING:
        if (ac > peakMax) {
            peakMax = ac;
            peakTick = now;
        }
        /* 回落到阈值30%以下 → 峰确认 (防双计数) */
        if (ac < thr * 30 / 100) {
            peakState = PEAK_IDLE;

            if (lastBeatTick > 0) {
                uint16_t ibi = (uint16_t)(peakTick - lastBeatTick);
                RecordIBI(ibi);
            }
            lastBeatTick = peakTick;

            /* 峰后衰减振幅 (加速适应) */
            acMax = acMax * 3 / 4;
            acMin = acMin * 3 / 4;
        }
        break;

    default:
        peakState = PEAK_IDLE;
        break;
    }

    /* ====== 7. 超时重置: 4秒无心搏 ====== */
    if (lastBeatTick > 0 && (now - lastBeatTick) > 4000) {
        peakState = PEAK_IDLE;
        peakMax = 0;
        acMax = 0;
        acMin = 0;
        ibiCnt = 0;
        ibiIdx = 0;
        lastBeatTick = now;
        heartBPM = 0;
        refracMs = 250;
        memset(ibiBuf, 0, sizeof(ibiBuf));
        memset(bpmHist, 0, sizeof(bpmHist));
        bpmHistIdx = 0;
        bpmHistCnt = 0;
        /* DC 用快通道重新收敛 */
        dcFast = adcValue;
        dcSampleCnt = 0;
    }
}
