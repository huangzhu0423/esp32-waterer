#pragma once
/*
 * web_server.h —— HTTP 服务与 REST 接口
 */
#include <stdint.h>

void webBegin(void);
void webTick(void);
void webRequestReboot(uint32_t delayMs);
