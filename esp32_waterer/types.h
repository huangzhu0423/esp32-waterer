#pragma once
/*
 * types.h —— 枚举与数据结构。不 include Arduino.h，保持可移植。
 */
#include <stdint.h>
#include "config.h"

/* ---------- 触发来源，值即优先级 ---------- */
enum TriggerSrc : uint8_t {
  SRC_NONE   = 0,
  SRC_AUTO   = 1,   /* 湿度低于阈值 */
  SRC_TIMER  = 2,   /* 定时任务 */
  SRC_MANUAL = 3    /* 网页手动 */
};

/* ---------- 停泵原因 ---------- */
enum StopReason : uint8_t {
  STOP_NONE = 0,
  STOP_DONE,        /* 正常浇完 */
  STOP_MANUAL,      /* 网页手动停止 */
  STOP_PREEMPT,     /* 被高优先级抢占 */
  STOP_TIMEOUT,     /* 撞到硬上限 */
  STOP_FAULT,       /* 传感器故障 */
  STOP_BUDGET,      /* 超出小时预算 */
  STOP_LOCKOUT,     /* 系统锁定 */
  STOP_SHUTDOWN
};

/* ---------- 传感器故障码 ---------- */
enum FaultCode : uint8_t {
  FLT_NONE  = 0,
  FLT_OPEN  = 1,    /* 断线 / 短路到地 */
  FLT_SHORT = 2,    /* 短路到 3V3 / 饱和 */
  FLT_STUCK = 3,    /* 读数卡死 */
  FLT_NOISY = 4,    /* 强噪声 */
  FLT_PWR   = 5     /* 探头供电异常：六路读数同时贴地，
                       基本可以断定是供电继电器没吸合 / 线圈断，
                       而不是六个探头一起坏。属于全局故障，会盖掉各路的原有状态。 */
};

/* ---------- 请求结果 ---------- */
enum ReqResult : uint8_t {
  REQ_OK = 0,
  REQ_ERR_FAULT,
  REQ_ERR_BUDGET,
  REQ_ERR_BUSY,
  REQ_ERR_COOLDOWN,
  REQ_ERR_LOCKOUT,
  REQ_ERR_INVALID
};

/* ---------- 网络模式 ---------- */
enum NetMode : uint8_t {
  NET_BOOT = 0,
  NET_STA_CONNECTING,
  NET_STA,
  NET_AP
};

/* ---------- 日志事件码 ---------- */
enum LogCode : uint8_t {
  LOG_BOOT = 0,
  LOG_RESET_REASON,
  LOG_PUMP_START,
  LOG_PUMP_DONE,
  LOG_PUMP_MANUAL_STOP,
  LOG_PUMP_PREEMPT,
  LOG_PUMP_TIMEOUT,
  LOG_PUMP_BUDGET,
  LOG_FAULT,
  LOG_RECOVER,
  LOG_PWR_FAIL,
  LOG_PWR_OK,
  LOG_CALIB,
  LOG_CFG_SAVE,
  LOG_WIFI_STA,
  LOG_WIFI_AP,
  LOG_WIFI_FAIL,
  LOG_TASK_FIRE,
  LOG_TASK_SKIP,
  LOG_LOCKOUT,
  LOG_CFG_BAD,
  LOG_COUNT
};

/* ---------- 单通道可配置项 ---------- */
struct ChannelCfg {
  uint8_t  autoOn;      /* 0/1 */
  uint8_t  thrPct;      /* 目标湿度阈值 5~90 */
  uint16_t maxSec;      /* 单次浇水时长，被 HARD_MAX_SEC 夹住 */
  uint16_t minGapSec;   /* 两次自动浇水的最小间隔 */
  uint16_t dryRaw;      /* 干点原始读数（大） */
  uint16_t wetRaw;      /* 湿点原始读数（小） */
};

/* ---------- 定时任务 ---------- */
struct TaskCfg {
  uint8_t  used;        /* 0 = 空槽 */
  uint8_t  on;
  uint8_t  ch;
  uint8_t  type;        /* 0 = 周期型，1 = 每日时刻型 */
  uint16_t periodMin;   /* 周期型：多少分钟一次 */
  uint16_t durSec;      /* 每次浇多少秒 */
  uint8_t  hour;        /* 每日型 */
  uint8_t  minute;
  uint32_t lastRunSec;  /* 上次运行的 uptime 秒（周期型用） */
};

/* ---------- 持久化整体 ---------- */
struct ConfigBlob {
  uint16_t   ver;
  ChannelCfg ch[CHANNEL_COUNT];
  TaskCfg    tasks[MAX_TASKS];
  char       ssid[33];
  char       pass[65];
  uint32_t   crc;
};
