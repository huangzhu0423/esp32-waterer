#pragma once
/*
 * logbuf.h —— RAM 环形日志。不写 flash，掉电即失。
 */
#include <stdint.h>
#include "types.h"

#define LOG_CAP 128

struct LogRec {
  uint32_t s;      /* 单调递增序号 */
  uint32_t t;      /* uptime 秒 */
  uint8_t  code;   /* LogCode */
  int8_t   ch;     /* -1 表示与通道无关 */
};

void        logBegin(void);
void        logAdd(uint8_t code, int8_t ch);
uint32_t    logSeq(void);
uint16_t    logSince(uint32_t sinceSeq, LogRec* out, uint16_t maxOut);
const char* logText(uint8_t code);
const char* logKind(uint8_t code);
