#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "types.h"
#include "scheduler.h"
#include "pump.h"
#include "sensors.h"
#include "health.h"
#include "config_store.h"
#include "logbuf.h"
#include "net.h"

/* ---------- 队列 ---------- */
struct QueuedItem {
  uint8_t  ch;
  uint8_t  src;      /* 当前优先级，可能因饥饿老化被提升 */
  uint8_t  orig;     /* 原始来源，永不变，用于区分"定时"和"被提升的自动" */
  uint32_t durMs;
  uint32_t atMs;
};

static QueuedItem s_q[CHANNEL_COUNT];
static uint8_t    s_qn = 0;

/* ---------- 自动触发的最小间隔 ---------- */
static uint32_t s_lastAutoMs[CHANNEL_COUNT];

/* ---------- 扫描节拍 ---------- */
static uint32_t s_lastAutoScanMs = 0;
static uint32_t s_lastTimerScanMs = 0;
static uint32_t s_lastDailyKey[MAX_TASKS];

void schedBegin(void) {
  s_qn = 0;
  memset(s_q, 0, sizeof(s_q));
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) s_lastAutoMs[i] = 0;
  for (uint8_t i = 0; i < MAX_TASKS; i++) s_lastDailyKey[i] = 0xFFFFFFFFUL;
  s_lastAutoScanMs = 0;
  s_lastTimerScanMs = 0;

  /* 周期型任务在重启后从 0 重新起算：
     如果 NVS 里的 lastRunSec 比当前 uptime 还大，说明是上一次开机留下的，
     直接按"刚跑过"处理会让它等满一个周期才浇，避免反复掉电时连续浇水。 */
  ConfigBlob& cfg = storeCfgRW();
  uint32_t nowSec = millis() / 1000UL;
  for (uint8_t i = 0; i < MAX_TASKS; i++) {
    if (!cfg.tasks[i].used) continue;
    if (cfg.tasks[i].lastRunSec > nowSec + 5) cfg.tasks[i].lastRunSec = nowSec;
  }
}

uint8_t schedQueueLen(void) { return s_qn; }

bool schedIsQueued(uint8_t ch) {
  for (uint8_t i = 0; i < s_qn; i++) {
    if (s_q[i].ch == ch) return true;
  }
  return false;
}

uint8_t schedPendingSrc(uint8_t ch) {
  for (uint8_t i = 0; i < s_qn; i++) {
    if (s_q[i].ch == ch) return s_q[i].src;
  }
  return SRC_NONE;
}

static void queueRemoveAt(uint8_t idx) {
  if (idx >= s_qn) return;
  for (uint8_t i = idx; i + 1 < s_qn; i++) s_q[i] = s_q[i + 1];
  s_qn--;
}

void schedCancel(uint8_t ch) {
  for (uint8_t i = 0; i < s_qn; ) {
    if (s_q[i].ch == ch) {
      queueRemoveAt(i);
    } else {
      i++;
    }
  }
}

/* ---------- 请求入口 ---------- */
uint8_t schedRequest(uint8_t ch, uint32_t durMs, uint8_t src) {
  if (ch >= CHANNEL_COUNT) return REQ_ERR_INVALID;
  if (durMs == 0) return REQ_ERR_INVALID;

  uint32_t nowMs = millis();
  const ChannelCfg& cc = storeCfg().ch[ch];

  /* ---- 1. 故障通道 ----
   * 断线 / 短路：根本测不到土里状态，自动、定时、手动一律拒绝。
   *   这不是保守，是唯一正确选择——此时开泵就是盲浇，淹死比旱死快得多。
   * 卡死 / 噪声：读数只是不可信，不是没有，仍放行手动让人现场判断。 */
  uint8_t flt = sensorsFault(ch);
  if (flt == FLT_OPEN || flt == FLT_SHORT) return REQ_ERR_FAULT;
  if (flt != FLT_NONE && src != SRC_MANUAL) return REQ_ERR_FAULT;

  /* ---- 2. 锁定期内不允许自动浇水 ---- */
  if (src != SRC_MANUAL && healthLocked(nowMs)) {
    return REQ_ERR_LOCKOUT;
  }

  /* ---- 3. 小时预算 ---- */
  if (!pumpBudgetLeft(ch, nowMs)) {
    return REQ_ERR_BUDGET;
  }

  /* ---- 4. 同一通道已经在跑或排队：刷新时长即可，不叠加 ---- */
  if (pumpIsActive() && pumpActiveCh() == (int8_t)ch) {
    if (src > pumpActiveSrc()) {
      /* 更高优先级请求同一路，让它继续跑，顺延时长由调用方决定 */
      return REQ_OK;
    }
    return REQ_OK;
  }
  if (schedIsQueued(ch)) return REQ_OK;

  /* ---- 5. 自动浇水的最小间隔 ---- */
  if (src == SRC_AUTO) {
    uint32_t gapMs = (uint32_t)cc.minGapSec * 1000UL;
    if (gapMs > 0 && s_lastAutoMs[ch] != 0 && (nowMs - s_lastAutoMs[ch]) < gapMs) {
      return REQ_ERR_COOLDOWN;
    }
  }

  /* ---- 6. 泵空闲：直接开 ---- */
  if (!pumpIsActive()) {
    /* 刚停泵的 2 秒内不开，给电源一个恢复窗口 */
    if ((nowMs - pumpLastStopMs()) < INTER_PUMP_GAP_MS && pumpLastStopMs() != 0) {
      /* 落到下面走排队 */
    } else {
      if (pumpStart(ch, durMs, src)) {
        if (src == SRC_AUTO) s_lastAutoMs[ch] = nowMs;
        return REQ_OK;
      }
      return REQ_ERR_BUSY;
    }
  }

  /* ---- 7. 泵被占用 ---- */
  if (pumpIsActive()) {
    uint8_t busySrc = pumpActiveSrc();
    uint32_t elapsed = pumpElapsedMs();

    if (src == SRC_MANUAL && busySrc < SRC_MANUAL) {
      if (elapsed < PREEMPT_MIN_MS) return REQ_ERR_BUSY;   /* 刚开就抢，容易抖，拒绝 */
      pumpStop(STOP_PREEMPT);
      /* 落到下面入队，下一个 tick 启动 */
    }
  }

  /* ---- 8. 入队 ---- */
  if (s_qn >= CHANNEL_COUNT) {
    /* 队列满：只有比队尾最低优先级更高才挤掉它 */
    uint8_t worst = 0;
    for (uint8_t i = 1; i < s_qn; i++) {
      if (s_q[i].src < s_q[worst].src) worst = i;
    }
    if (src <= s_q[worst].src) return REQ_ERR_BUSY;
    queueRemoveAt(worst);
  }

  s_q[s_qn].ch = ch;
  s_q[s_qn].src = src;
  s_q[s_qn].orig = src;
  s_q[s_qn].durMs = durMs;
  s_q[s_qn].atMs = nowMs;
  s_qn++;
  if (src == SRC_AUTO) s_lastAutoMs[ch] = nowMs;
  return REQ_OK;
}

/* ---------- 挑出队首 ---------- */
static int8_t pickTop(uint32_t nowMs) {
  int8_t best = -1;
  for (uint8_t i = 0; i < s_qn; i++) {
    /* 跳过故障通道与超预算通道 */
    if (sensorsFault(s_q[i].ch) != FLT_NONE) continue;
    if (!pumpBudgetLeft(s_q[i].ch, nowMs)) continue;

    if (best < 0) { best = (int8_t)i; continue; }
    if (s_q[i].src > s_q[best].src) best = (int8_t)i;
    else if (s_q[i].src == s_q[best].src && s_q[i].atMs < s_q[best].atMs) best = (int8_t)i;
  }
  return best;
}

/* ---------- 自动触发扫描 ---------- */
static void autoScan(uint32_t nowMs) {
  if (healthLocked(nowMs)) return;
  if (pumpIsActive()) return;

  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    const ChannelCfg& cc = storeCfg().ch[i];
    if (!cc.autoOn) continue;
    if (sensorsFault(i) != FLT_NONE) continue;
    if (!sensorsReady(i)) continue;
    if (schedIsQueued(i)) continue;
    if (sensorsPct(i) >= cc.thrPct) continue;

    /* 越干浇越久：阈值以下每低 10 个百分点加 50%，最多翻倍 */
    uint32_t durMs = (uint32_t)cc.maxSec * 1000UL;
    uint8_t deficit = (uint8_t)(cc.thrPct - sensorsPct(i));
    uint32_t boost = 100UL + (uint32_t)(deficit / 10) * 50UL;
    durMs = durMs * boost / 100UL;

    schedRequest(i, durMs, SRC_AUTO);
    break;                      /* 一次只触发一路，避免瞬间全排上队 */
  }
}

/* ---------- 定时任务扫描 ---------- */
static void timerScan(uint32_t nowMs) {
  ConfigBlob& cfg = storeCfgRW();
  uint32_t nowSec = nowMs / 1000UL;
  bool timeOk = netTimeSynced();

  for (uint8_t i = 0; i < MAX_TASKS; i++) {
    TaskCfg& t = cfg.tasks[i];
    if (!t.used || !t.on) continue;
    if (t.ch >= CHANNEL_COUNT) continue;

    bool due = false;

    if (t.type == 0) {
      /* 周期型：完全不依赖墙上时间，掉电重启照样算 */
      uint32_t period = (uint32_t)t.periodMin * 60UL;
      if (period < 60) period = 60;
      uint32_t last = t.lastRunSec;
      if (nowSec >= last && (nowSec - last) >= period) due = true;
    } else {
      /* 每日时刻型：必须时间已同步 */
      if (!timeOk) continue;
      time_t nowT = time(nullptr);
      struct tm tmv;
      localtime_r(&nowT, &tmv);
      if (tmv.tm_hour == (int)t.hour && tmv.tm_min == (int)t.minute) {
        uint32_t key = (uint32_t)(tmv.tm_yday * 1440 + tmv.tm_hour * 60 + tmv.tm_min);
        if (s_lastDailyKey[i] != key) due = true;
      }
    }

    if (!due) continue;

    uint8_t r = schedRequest(t.ch, (uint32_t)t.durSec * 1000UL, SRC_TIMER);
    if (r == REQ_OK) {
      t.lastRunSec = nowSec;
      if (t.type == 1) {
        time_t nowT = time(nullptr);
        struct tm tmv;
        localtime_r(&nowT, &tmv);
        s_lastDailyKey[i] = (uint32_t)(tmv.tm_yday * 1440 + tmv.tm_hour * 60 + tmv.tm_min);
      }
      logAdd(LOG_TASK_FIRE, (int8_t)t.ch);
      storeTouch();
    } else if (r == REQ_ERR_FAULT || r == REQ_ERR_BUDGET || r == REQ_ERR_LOCKOUT) {
      /* 这次没法浇，把 lastRunSec 往后推，避免同一秒反复重试 */
      t.lastRunSec = nowSec;
      logAdd(LOG_TASK_SKIP, (int8_t)t.ch);
    }
  }
}

/* ---------- 主 tick ---------- */
void schedTick(uint32_t nowMs) {
  /* 1. 泵到时自己停 */
  pumpTick(nowMs);

  /* 2. 老化：排队太久的自动请求提一级，防止手动一直抢导致自动饿死。
        注意只提优先级，不改变 orig，否则下面第 3 步会把它当定时任务丢掉。 */
  for (uint8_t i = 0; i < s_qn; i++) {
    if (s_q[i].orig == SRC_AUTO && s_q[i].src < SRC_TIMER &&
        (nowMs - s_q[i].atMs) > MAX_WAIT_MS) {
      s_q[i].src = SRC_TIMER;
    }
  }

  /* 3. 真正的定时任务超过等待窗口直接丢弃：错过时间点的浇水没有意义。
        判断用 orig，被提升上来的自动请求不算。 */
  for (uint8_t i = 0; i < s_qn; ) {
    if (s_q[i].orig == SRC_TIMER && (nowMs - s_q[i].atMs) > MAX_WAIT_MS) {
      logAdd(LOG_TASK_SKIP, (int8_t)s_q[i].ch);
      queueRemoveAt(i);
    } else {
      i++;
    }
  }

  /* 4. 空闲时取队首启动 */
  if (!pumpIsActive() && s_qn > 0) {
    if (pumpLastStopMs() == 0 || (nowMs - pumpLastStopMs()) >= INTER_PUMP_GAP_MS) {
      int8_t top = pickTop(nowMs);
      if (top >= 0) {
        uint8_t ch = s_q[top].ch;
        uint32_t dur = s_q[top].durMs;
        uint8_t src = s_q[top].src;
        queueRemoveAt((uint8_t)top);
        if (!pumpStart(ch, dur, src)) {
          uint8_t f = sensorsFault(ch);
          if (f == FLT_OPEN || f == FLT_SHORT) {
            /* 传感器彻底失效：退回队列会每个 tick 重试一次、永远起不来，
               还占着队首把其他通道全饿死。直接丢弃，并记一条日志。 */
            logAdd(LOG_TASK_SKIP, (int8_t)ch);
          } else if (s_qn < CHANNEL_COUNT) {
            /* 起不来就退回队列头，下个 tick 再试。
               orig 必须一起恢复：老化提升和超时丢弃都靠它区分来源，
               漏掉会让这条请求被误判成定时任务而提前丢弃。 */
            for (uint8_t k = s_qn; k > 0; k--) s_q[k] = s_q[k - 1];
            s_q[0].ch = ch; s_q[0].durMs = dur; s_q[0].src = src;
            s_q[0].orig = src; s_q[0].atMs = nowMs;
            s_qn++;
          }
        }
      } else {
        /* 队里全是故障或超预算通道，清掉免得反复扫描 */
        for (uint8_t i = 0; i < s_qn; ) {
          if (sensorsFault(s_q[i].ch) != FLT_NONE || !pumpBudgetLeft(s_q[i].ch, nowMs)) {
            queueRemoveAt(i);
          } else {
            i++;
          }
        }
      }
    }
  }

  /* 5. 周期性扫描 */
  if (nowMs - s_lastAutoScanMs >= 5000UL) {
    s_lastAutoScanMs = nowMs;
    autoScan(nowMs);
  }
  if (nowMs - s_lastTimerScanMs >= 30000UL) {
    s_lastTimerScanMs = nowMs;
    timerScan(nowMs);
  }
}
