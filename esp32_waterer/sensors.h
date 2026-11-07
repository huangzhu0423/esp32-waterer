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

/* 请求立刻补采一轮。浇水结束后用：土湿已经变了，
   等下一个 30 分钟周期才更新的话，期间的自动判断都拿着旧读数。 */
void     sensorsRequestNow(void);

/* 当前是否正处于上电采集窗口 */
bool     sensorsSampling(void);

/* 探头供电是否异常（六路读数同时贴地）。为 true 时各通道的 flt 都是 FLT_PWR，
   表示供电没上去，而不是六个探头一起坏了。 */
bool     sensorsPwrFault(void);

/* 距下一轮采样还有多久（毫秒）；正在采或首轮未完成时返回 0 */
uint32_t sensorsNextCycleMs(uint32_t nowMs);

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
