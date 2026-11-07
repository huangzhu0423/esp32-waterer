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
  uint16_t d;      /* 附带秒数：开始浇水记设定时长，停止浇水记实际时长，其余为 0 */
};

void        logBegin(void);
/* durSec 默认 0，现有调用点不用改；只有浇水类日志传秒数 */
void        logAdd(uint8_t code, int8_t ch, uint16_t durSec = 0);
uint32_t    logSeq(void);
uint16_t    logSince(uint32_t sinceSeq, LogRec* out, uint16_t maxOut);
const char* logText(uint8_t code);
const char* logKind(uint8_t code);
