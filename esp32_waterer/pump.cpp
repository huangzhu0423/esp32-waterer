#include <Arduino.h>

#include "config.h"
#include "types.h"
#include "pump.h"
#include "sensors.h"
#include "config_store.h"
#include "logbuf.h"

static int8_t   s_activeCh = -1;
static uint8_t  s_src = SRC_NONE;
static uint32_t s_startMs = 0;
static uint32_t s_durMs = 0;
static uint32_t s_lastStopMs = 0;
static uint32_t s_lastEdgeMs = 0;

/* ---- 小时预算：按整点小时分桶，换桶即清零 ---- */
static uint32_t s_winHour[CHANNEL_COUNT];
static uint32_t s_usedSec[CHANNEL_COUNT];
static uint32_t s_globHour = 0;
static uint32_t s_globUsedSec = 0;

/* ============ 输出层：唯一直接碰硬件的地方 ============
 * 用位掩码整体下发，而不是逐路开关。原因是换成 MCP23017 后，
 * 它的输出是"读-改-写整个 16 位寄存器"，逐路接口没法一一映射，
 * 而位掩码可以整块翻译过去。
 * bit i = 1 表示通道 i 开泵。
 */
static uint32_t s_outMask = 0;

static void pumpHwApply(uint32_t mask) {
#if USE_IO_EXPANDER
  /* 32 路时在这里换成 MCP23017（I2C 写两片 16 位寄存器）
     或 74HC595（移位串出 4 个字节）。
     上层调用方式与下面完全一致，不需要任何改动。 */
  (void)mask;
#else
  for (uint8_t i = 0; i < CHANNEL_COUNT && i < 32; i++) {
    digitalWrite(PIN_PUMP[i], (mask & (1UL << i)) ? PUMP_ON_LEVEL : PUMP_OFF_LEVEL);
  }
#endif
}

static void rollBucket(uint32_t nowMs) {
  uint32_t h = nowMs / 3600000UL;
  if (h != s_globHour) {
    s_globHour = h;
    s_globUsedSec = 0;
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
      s_winHour[i] = h;
      s_usedSec[i] = 0;
    }
  } else {
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
      if (s_winHour[i] != h) {
        s_winHour[i] = h;
        s_usedSec[i] = 0;
      }
    }
  }
}

void pumpBegin(void) {
  /* 上电第一件事：把所有泵脚拉到"关"电平再设为输出。
     注意 bootloader 有 200~300ms 的窗口软件管不到，
     那段时间靠硬件 10k 下拉（低边 MOS）或模块自带上拉（继电器）保证不动作。 */
#if !USE_IO_EXPANDER
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    digitalWrite(PIN_PUMP[i], PUMP_OFF_LEVEL);   /* 先写输出寄存器，避免切换瞬间冒高电平 */
    pinMode(PIN_PUMP[i], OUTPUT);
    digitalWrite(PIN_PUMP[i], PUMP_OFF_LEVEL);   /* 切换后再确认一次 */
  }
#endif
  s_outMask = 0;
  pumpHwApply(0);
  s_activeCh = -1;
  s_src = SRC_NONE;
  s_startMs = 0;
  s_durMs = 0;
  s_lastStopMs = 0;
  s_lastEdgeMs = 0;
  s_globHour = 0;
  s_globUsedSec = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    s_winHour[i] = 0;
    s_usedSec[i] = 0;
  }
}

bool pumpIsActive(void) { return s_activeCh >= 0; }
int8_t pumpActiveCh(void) { return s_activeCh; }
uint8_t pumpActiveSrc(void) { return s_src; }
uint32_t pumpLastStopMs(void) { return s_lastStopMs; }
uint32_t pumpActiveDurMs(void) { return s_durMs; }

uint32_t pumpElapsedMs(void) {
  if (s_activeCh < 0) return 0;
  uint32_t n = millis();
  return (n >= s_startMs) ? (n - s_startMs) : 0;
}

uint32_t pumpRemainMs(void) {
  if (s_activeCh < 0) return 0;
  uint32_t e = pumpElapsedMs();
  return (e >= s_durMs) ? 0 : (s_durMs - e);
}

bool pumpEdgeRecent(uint32_t nowMs) {
  return (nowMs - s_lastEdgeMs) < ADC_SETTLE_MS;
}

bool pumpStart(uint8_t ch, uint32_t durMs, uint8_t src) {
  if (ch >= CHANNEL_COUNT) return false;

  /* 互斥断言：已经有泵在跑就绝不开启第二路，防止 5V/2A 电源被拉爆 */
  if (s_activeCh >= 0) return false;

  /* 最后一道闸门：传感器彻底失效时，任何来源都不许开泵。
     schedRequest / pickTop / autoScan 已经层层拦过，这里再兜一次底，
     因为 pumpStart 是所有执行路径唯一的汇聚点——将来新增任何入口
     （HTTP 直连、串口命令、定时器回调）都绕不过这一行。
     断线和短路意味着完全不知道土里干不干，此时浇水就是盲浇，比不浇更危险。 */
  uint8_t flt = sensorsFault(ch);
  if (flt == FLT_OPEN || flt == FLT_SHORT) {
    /* 这里静默返回、不写日志：调用方 schedTick 会把失败的请求退回队列头
       下个 tick 重试，若在此处记日志，每个 loop 都会刷一条，几秒就冲爆日志环。
       日志由 schedTick 在决定丢弃时统一记一次。 */
    return false;
  }

  if (durMs == 0) return false;
  if (durMs > (uint32_t)HARD_MAX_SEC * 1000UL) {
    durMs = (uint32_t)HARD_MAX_SEC * 1000UL;   /* 硬上限，配置改不动它 */
  }

  s_activeCh = (int8_t)ch;
  s_src = src;
  s_startMs = millis();
  s_durMs = durMs;
  s_lastEdgeMs = s_startMs;

  s_outMask |= (1UL << ch);
  pumpHwApply(s_outMask);                      /* 开泵（有效电平由 PUMP_ACTIVE_LOW 决定） */
  storeRunMarkerSet(true);                     /* 标记"带泵运行中"，用于掉电检测 */
  /* 记下本次设定时长，远程页能直接核对「设置 15 秒是否真按 15 秒浇」 */
  logAdd(LOG_PUMP_START, (int8_t)ch, (uint16_t)(durMs / 1000UL));
  return true;
}

void pumpStop(uint8_t reason) {
  if (s_activeCh < 0) return;

  uint8_t ch = (uint8_t)s_activeCh;
  uint32_t nowMs = millis();
  uint32_t ran = (nowMs >= s_startMs) ? (nowMs - s_startMs) : 0;

  s_outMask &= ~(1UL << ch);
  pumpHwApply(s_outMask);
  s_lastEdgeMs = nowMs;
  s_lastStopMs = nowMs;
  s_activeCh = -1;

  rollBucket(nowMs);
  uint32_t ranSec = ran / 1000UL;
  s_usedSec[ch] += ranSec;
  s_globUsedSec += ranSec;
  uint16_t ranSec16 = (uint16_t)(ranSec > 65535UL ? 65535UL : ranSec);

  storeRunMarkerSet(false);

  /* 停止日志带上实际运行秒数：设定 15s 却跑了 300s 时一眼能看出来 */
  switch (reason) {
    case STOP_DONE:    logAdd(LOG_PUMP_DONE, (int8_t)ch, ranSec16);        break;
    case STOP_MANUAL:  logAdd(LOG_PUMP_MANUAL_STOP, (int8_t)ch, ranSec16); break;
    case STOP_PREEMPT: logAdd(LOG_PUMP_PREEMPT, (int8_t)ch, ranSec16);     break;
    case STOP_TIMEOUT: logAdd(LOG_PUMP_TIMEOUT, (int8_t)ch, ranSec16);     break;
    case STOP_BUDGET:  logAdd(LOG_PUMP_BUDGET, (int8_t)ch, ranSec16);      break;
    default: break;
  }
  s_src = SRC_NONE;

  /* 土刚被浇过，湿度已经变了。请求尽快补采一轮，
     否则自动逻辑会拿着"浇水前"的旧读数继续判断，可能马上浇第二次。
     sensorsRequestNow 内部有 60 秒节流，连续浇水不会反复开采样。 */
  sensorsRequestNow();
}

void pumpTick(uint32_t nowMs) {
  if (s_activeCh < 0) return;

  uint32_t el = (nowMs >= s_startMs) ? (nowMs - s_startMs) : 0;

  /* 冗余保护：即使调用方算错时长，这里也一定在 5 分钟处切断 */
  if (el >= (uint32_t)HARD_MAX_SEC * 1000UL + 1000UL) {
    pumpStop(STOP_TIMEOUT);
    return;
  }
  if (el >= s_durMs) {
    pumpStop(STOP_DONE);
  }
}

bool pumpBudgetLeft(uint8_t ch, uint32_t nowMs) {
  if (ch >= CHANNEL_COUNT) return false;
  rollBucket(nowMs);
  if (s_usedSec[ch] >= CH_BUDGET_SEC_PER_HOUR) return false;
  if (s_globUsedSec >= GLOBAL_BUDGET_SEC_PER_HOUR) return false;
  return true;
}

uint32_t pumpBudgetUsedSec(uint8_t ch, uint32_t nowMs) {
  if (ch >= CHANNEL_COUNT) return 0;
  rollBucket(nowMs);
  return s_usedSec[ch];
}
