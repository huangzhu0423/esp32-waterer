#pragma once
/*
 * mqtt_link.h —— MQTT 远程控制链路
 *
 * 设计要点（与项目整体风格一致）：
 *   - 非阻塞：所有动作都在 mqttTick() 里按状态机推进，绝不 delay() 卡死主循环
 *   - 配置独立：走 NVS 命名空间 "mqtt"，不碰 ConfigBlob，改 MQTT 不会重置主配置
 *   - 安全边界：命令必须带正确令牌（token）才执行，否则直接忽略；
 *     令牌为空时放行（仅适合完全隔离的私有 broker）
 *   - 只连 STA：没有路由器（netMode != NET_STA）时根本不尝试，避免空耗
 */
#include <stdint.h>

void     mqttBegin(void);
void     mqttTick(uint32_t nowMs);

bool     mqttEnabled(void);
bool     mqttConnected(void);
void     mqttPublishState(void);   /* 立即上报一次（外部触发器调用） */

/* ---------- 配置读写（Web 接口用） ---------- */
bool     mqttCfgEnabled(void);
void     mqttCfgEnabledSet(bool on);
const char* mqttCfgHost(void);
void     mqttCfgHostSet(const char* h);
uint16_t mqttCfgPort(void);
void     mqttCfgPortSet(uint16_t p);
const char* mqttCfgUser(void);
void     mqttCfgUserSet(const char* u);
const char* mqttCfgPass(void);
void     mqttCfgPassSet(const char* p);
const char* mqttCfgToken(void);
void     mqttCfgTokenSet(const char* t);
const char* mqttCfgBase(void);
void     mqttCfgBaseSet(const char* b);
