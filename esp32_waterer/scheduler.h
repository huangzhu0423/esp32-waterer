#pragma once
/*
 * scheduler.h —— 三源请求仲裁：手动 > 定时 > 自动
 * 全局互斥：任何时刻最多一路泵运行，其余排队。
 */
#include <stdint.h>

void    schedBegin(void);

/* 返回 ReqResult */
uint8_t schedRequest(uint8_t ch, uint32_t durMs, uint8_t src);
void    schedTick(uint32_t nowMs);

uint8_t schedQueueLen(void);
bool    schedIsQueued(uint8_t ch);
uint8_t schedPendingSrc(uint8_t ch);

/* 撤回某通道还在排队的请求（已在运行的由 pumpStop 处理） */
void    schedCancel(uint8_t ch);
