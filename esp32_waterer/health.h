#pragma once
/*
 * health.h —— 看门狗、复位原因、异常掉电锁定
 */
#include <stdint.h>

void        healthBegin(void);
void        healthTick(uint32_t nowMs);

/* 带泵复位后的一段冷却期，期间禁止自动/定时浇水 */
bool        healthLocked(uint32_t nowMs);
uint32_t    healthLockRemainSec(uint32_t nowMs);

void        healthWatchdogBegin(void);
void        healthWatchdogFeed(void);

uint8_t     healthResetReason(void);
const char* healthResetText(void);
