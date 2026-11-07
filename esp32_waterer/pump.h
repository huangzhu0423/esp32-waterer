#pragma once
/*
 * pump.h —— 水泵驱动。整个固件里唯一允许写水泵引脚的地方。
 * 互斥保证：任何时刻最多一路处于开启状态，由这里的单一令牌强制。
 */
#include <stdint.h>

void     pumpBegin(void);
bool     pumpStart(uint8_t ch, uint32_t durMs, uint8_t src);
void     pumpStop(uint8_t reason);
void     pumpTick(uint32_t nowMs);

bool     pumpIsActive(void);
int8_t   pumpActiveCh(void);
uint8_t  pumpActiveSrc(void);
uint32_t pumpRemainMs(void);
uint32_t pumpElapsedMs(void);
uint32_t pumpLastStopMs(void);
uint32_t pumpActiveDurMs(void);

/* 泵启停后的地弹窗口判断，供传感器采样避开 */
bool     pumpEdgeRecent(uint32_t nowMs);

/* 小时预算 */
bool     pumpBudgetLeft(uint8_t ch, uint32_t nowMs);
uint32_t pumpBudgetUsedSec(uint8_t ch, uint32_t nowMs);
