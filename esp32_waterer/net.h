#pragma once
/*
 * net.h —— 非阻塞 WiFi 状态机
 * 策略：AP 常开（AutoWater-XXXX），同时后台尝试连家里路由器。
 * AP 常开是为了避免"连不上路由器就再也进不去"的死局，不需要拔线重刷。
 */
#include <stdint.h>
#include <stddef.h>

void        netBegin(void);
void        netTick(uint32_t nowMs);

uint8_t     netMode(void);
void        netIp(char* buf, size_t n);
int         netRssi(void);

bool        netTimeSynced(void);
uint32_t    netEpoch(void);

const char* netApSsid(void);
void        netSetCredentials(const char* ssid, const char* pass);

/* 改热点密码后必须调这个才生效：softAP 的密码在启动时就固定了。
 * 副作用是会踢掉所有已连客户端，包括正在提交请求的那台设备。 */
void        netRestartAp(void);
