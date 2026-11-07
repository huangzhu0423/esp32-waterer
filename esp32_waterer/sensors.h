#pragma once
/*
 * sensors.h —— 土壤湿度采样、校准换算、故障判定
 *
 * 对外只暴露 readRaw 语义的接口，将来换 CD74HC4067 多路复用器
 * 只需要改 sensorsReadRaw() 一个函数。
 */
#include <stdint.h>

void     sensorsBegin(void);
void     sensorsTick(uint32_t nowMs, bool pumpRecent);

uint16_t sensorsRaw(uint8_t ch);
uint8_t  sensorsPct(uint8_t ch);
uint8_t  sensorsFault(uint8_t ch);
bool     sensorsReady(uint8_t ch);

/* 校准：取当前 9 次采样的中位数作为干点或湿点 */
uint16_t sensorsMedianNow(uint8_t ch);
void     sensorsApplyCalib(uint8_t ch, uint16_t dryRaw, uint16_t wetRaw);
void     sensorsRecomputePct(void);

/* 一键标定用的探测：连采 3 组 9 次中位数再取中位数，比单次稳。
 * st 输出 0 = 读数可用，FLT_OPEN / FLT_SHORT = 探头没接好，raw 不能拿去标定。 */
uint16_t sensorsCalibProbe(uint8_t ch, uint8_t* st);

void     sensorsForceSample(uint8_t ch);
