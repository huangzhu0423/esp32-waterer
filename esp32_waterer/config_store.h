#pragma once
/*
 * config_store.h —— 配置持久化（NVS / Preferences）+ CRC 校验 + 去抖落盘
 */
#include "types.h"

void               storeBegin(void);
const ConfigBlob&  storeCfg(void);
ConfigBlob&        storeCfgRW(void);

void storeDefaults(ConfigBlob& c);
void storeTouch(void);            /* 标记脏，2 秒后自动落盘 */
void storeTick(uint32_t nowMs);
void storeFlush(void);            /* 立即落盘 */

uint32_t storeCrc(const ConfigBlob& c);

/* 热点密码：单独一个 NVS key，不塞进 ConfigBlob。
 * 返回的指针指向静态缓冲，调用方不要缓存，也不要改。 */
const char* storeApPass(void);
void        storeApPassSet(const char* p);   /* 立即落盘，调用方负责校验长度 */

/* 采样周期（分钟）。同样走独立 key，改这个不会触发配置重置。 */
uint16_t storeSampleCycle(void);
void     storeSampleCycleSet(uint16_t min);  /* 越界的值直接忽略 */

/* 泵启动标记：用于识别"带泵复位" */
void storeRunMarkerSet(bool on);
bool storeRunMarkerGet(void);
uint8_t storeBadBootCount(void);
void storeBadBootCountSet(uint8_t n);
