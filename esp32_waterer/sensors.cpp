#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "types.h"
#include "sensors.h"
#include "config_store.h"
#include "logbuf.h"

/* ---------- 每通道运行态 ---------- */
static uint16_t s_raw[CHANNEL_COUNT];
static uint8_t  s_pct[CHANNEL_COUNT];
static uint8_t  s_fault[CHANNEL_COUNT];
static bool     s_ready[CHANNEL_COUNT];

static uint32_t s_lastSampleMs[CHANNEL_COUNT];

static uint8_t  s_badCnt[CHANNEL_COUNT];     /* 连续异常样本 */
static uint8_t  s_goodCnt[CHANNEL_COUNT];    /* 连续正常样本，用于解除故障 */
static uint8_t  s_rejStreak[CHANNEL_COUNT];  /* 连续被跳变过滤掉的次数 */
static uint8_t  s_noiseCnt[CHANNEL_COUNT];   /* 最近一分钟丢弃数 */
static uint32_t s_noiseWinMs[CHANNEL_COUNT];
static uint8_t  s_sameCnt[CHANNEL_COUNT];    /* 连续完全相同的读数，用于卡死判定 */

static uint8_t  s_rr = 0;                    /* 轮询游标 */
static uint16_t s_lastGood[CHANNEL_COUNT];   /* 0xFFFF = 还没有有效读数 */

/* ============ 底层读取（将来换 CD74HC4067 只改这里） ============ */
static uint16_t sensorsReadRaw(uint8_t ch) {
#if USE_MUX
  digitalWrite(PIN_MUX_S0, (ch >> 0) & 1);
  digitalWrite(PIN_MUX_S1, (ch >> 1) & 1);
  digitalWrite(PIN_MUX_S2, (ch >> 2) & 1);
  digitalWrite(PIN_MUX_S3, (ch >> 3) & 1);
  delayMicroseconds(5);
  return (uint16_t)analogRead(PIN_MUX_SIG);
#else
  return (uint16_t)analogRead(PIN_ADC[ch]);
#endif
}

/* 9 次采样取中位数：抑制 WiFi 发射瞬间的尖峰 */
static uint16_t readMedian(uint8_t ch) {
  uint16_t v[ADC_OVERSAMPLE];
  for (uint8_t i = 0; i < ADC_OVERSAMPLE; i++) v[i] = sensorsReadRaw(ch);

  /* 插入排序，只有 9 个样本，够快 */
  for (uint8_t i = 1; i < ADC_OVERSAMPLE; i++) {
    uint16_t t = v[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && v[j] > t) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = t;
  }
  return v[ADC_OVERSAMPLE / 2];
}

/* ============ 百分比换算 ============ */
static uint8_t rawToPct(uint8_t ch, uint16_t raw) {
  const ChannelCfg& c = storeCfg().ch[ch];
  int32_t dry = (int32_t)c.dryRaw;
  int32_t wet = (int32_t)c.wetRaw;
  int32_t span = dry - wet;
  if (span < 100) span = 100;               /* 校准异常兜底，避免除零 */
  int32_t p = ((dry - (int32_t)raw) * 100) / span;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (uint8_t)p;
}

void sensorsRecomputePct(void) {
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (s_ready[i]) s_pct[i] = rawToPct(i, s_raw[i]);
  }
}

/* ============ 故障标记 ============ */
static void raiseFault(uint8_t ch, uint8_t code) {
  if (s_fault[ch] == code) return;
  s_fault[ch] = code;
  logAdd(LOG_FAULT, (int8_t)ch);
}

static void clearFault(uint8_t ch) {
  if (s_fault[ch] == FLT_NONE) return;
  s_fault[ch] = FLT_NONE;
  logAdd(LOG_RECOVER, (int8_t)ch);
}

/* ============ 单路采样 + 质量判定 ============ */
static void sampleChannel(uint8_t ch, uint32_t nowMs) {
  uint16_t raw = readMedian(ch);

  /* ---- 1. 单次跳变过滤 ----
   * 连续 3 次都超阈值说明不是噪声而是真实变化（比如探头被拔出又插回），
   * 此时必须重新同步，否则会永远拒绝下去、通道假死在旧值上。 */
  if (s_lastGood[ch] != 0xFFFF) {
    int32_t d = (int32_t)raw - (int32_t)s_lastGood[ch];
    if (d < 0) d = -d;
    if (d > ADC_JUMP_RAW) {
      if (s_rejStreak[ch] < 250) s_rejStreak[ch]++;

      /* 一分钟滚动窗口内的丢弃计数 */
      if (nowMs - s_noiseWinMs[ch] > 60000UL) {
        s_noiseCnt[ch] = 0;
        s_noiseWinMs[ch] = nowMs;
      }
      s_noiseCnt[ch]++;

      if (s_rejStreak[ch] < 3) {
        if (s_noiseCnt[ch] > NOISY_LIMIT_PER_MIN) raiseFault(ch, FLT_NOISY);
        return;                       /* 丢弃这个样本 */
      }
      /* 连续 3 次都跳变 → 认定真实变化，重新同步后继续 */
      s_rejStreak[ch] = 0;
    } else {
      s_rejStreak[ch] = 0;
    }
  }

  /* ---- 2. 断线 / 短路 ----
   * 100k 泄放电阻保证探头拔掉时读数落到 0 附近，
   * 因此 raw ≈ 0 可以和"干土"区分开（干土在 2800~3300）。 */
  if (raw < ADC_OPEN_RAW) {
    s_raw[ch] = raw;
    s_ready[ch] = true;
    s_pct[ch] = 0;
    s_goodCnt[ch] = 0;
    s_sameCnt[ch] = 0;
    if (s_badCnt[ch] < 255) s_badCnt[ch]++;
    if (s_badCnt[ch] >= FAULT_CONFIRM_COUNT) raiseFault(ch, FLT_OPEN);
    s_lastGood[ch] = raw;
    return;
  }
  if (raw > ADC_SHORT_RAW) {
    s_raw[ch] = raw;
    s_ready[ch] = true;
    s_pct[ch] = 0;
    s_goodCnt[ch] = 0;
    s_sameCnt[ch] = 0;
    if (s_badCnt[ch] < 255) s_badCnt[ch]++;
    if (s_badCnt[ch] >= FAULT_CONFIRM_COUNT) raiseFault(ch, FLT_SHORT);
    s_lastGood[ch] = raw;
    return;
  }

  /* ---- 3. 卡死判定 ----
   * 土湿在几十秒内本来就几乎不变，所以不能用"变化太小"来判卡死（会误报）。
   * 改用"连续 N 次完全相同的整数读数"：真实 ADC 至少有 ±1 LSB 抖动，
   * 连续 60 次（约 30 秒）一字不差才是真卡死。 */
  if (s_lastGood[ch] == raw && s_sameCnt[ch] < 255) {
    s_sameCnt[ch]++;
  } else {
    s_sameCnt[ch] = 0;
  }

  s_raw[ch] = raw;
  s_ready[ch] = true;
  s_pct[ch] = rawToPct(ch, raw);
  s_lastGood[ch] = raw;

  if (s_sameCnt[ch] >= STUCK_SAMPLES) {
    raiseFault(ch, FLT_STUCK);
    s_sameCnt[ch] = 0;
    return;
  }

  /* ---- 4. 正常：累计恢复计数 ---- */
  s_badCnt[ch] = 0;
  if (s_goodCnt[ch] < 255) s_goodCnt[ch]++;
  if (s_fault[ch] != FLT_NONE && s_goodCnt[ch] >= FAULT_RECOVER_COUNT) {
    clearFault(ch);
    s_noiseCnt[ch] = 0;
  }
}

/* ============ 对外接口 ============ */
void sensorsBegin(void) {
  analogReadResolution(12);
  /* Arduino-ESP32 的默认衰减就是 11dB（0~3.3V 量程），这里不显式调用
     analogSetAttenuation()，避免 ADC_11db / ADC_ATTEN_DB_12
     在 core 2.x 与 3.x 上的命名差异导致编译失败。 */

  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    pinMode(PIN_ADC[i], INPUT);       /* 36/39/34/35 是 input-only，这行只为语义清晰 */
    s_raw[i] = 0;
    s_pct[i] = 0;
    s_fault[i] = FLT_NONE;
    s_ready[i] = false;
    s_lastSampleMs[i] = 0;
    s_badCnt[i] = 0;
    s_goodCnt[i] = 0;
    s_rejStreak[i] = 0;
    s_noiseCnt[i] = 0;
    s_noiseWinMs[i] = 0;
    s_sameCnt[i] = 0;
    s_lastGood[i] = 0xFFFF;
  }
  s_rr = 0;
}

void sensorsForceSample(uint8_t ch) {
  if (ch >= CHANNEL_COUNT) return;
  s_lastSampleMs[ch] = millis();
  sampleChannel(ch, millis());
}

void sensorsTick(uint32_t nowMs, bool pumpRecent) {
  /* 泵启停后的地弹窗口内不采样，否则会产生假跳变 */
  if (pumpRecent) return;

  for (uint8_t k = 0; k < CHANNEL_COUNT; k++) {
    uint8_t ch = (uint8_t)((s_rr + k) % CHANNEL_COUNT);
    if (nowMs - s_lastSampleMs[ch] >= ADC_SAMPLE_INTERVAL_MS) {
      s_lastSampleMs[ch] = nowMs;
      s_rr = (uint8_t)((ch + 1) % CHANNEL_COUNT);
      sampleChannel(ch, nowMs);
      return;                        /* 一次 tick 只采一路，避免占用 loop 太久 */
    }
  }
}

uint16_t sensorsRaw(uint8_t ch) {
  return (ch < CHANNEL_COUNT) ? s_raw[ch] : 0;
}

uint8_t sensorsPct(uint8_t ch) {
  return (ch < CHANNEL_COUNT) ? s_pct[ch] : 0;
}

uint8_t sensorsFault(uint8_t ch) {
  return (ch < CHANNEL_COUNT) ? s_fault[ch] : (uint8_t)FLT_OPEN;
}

bool sensorsReady(uint8_t ch) {
  return (ch < CHANNEL_COUNT) ? s_ready[ch] : false;
}

uint16_t sensorsMedianNow(uint8_t ch) {
  if (ch >= CHANNEL_COUNT) return 0;
  return readMedian(ch);
}

uint16_t sensorsCalibProbe(uint8_t ch, uint8_t* st) {
  if (st) *st = FLT_OPEN;
  if (ch >= CHANNEL_COUNT) return 0;

  /* 单次 9 采样容易被 WiFi 发射尖峰带偏，标定要写进 NVS、以后一直用，
     所以多采两组再取一次中位数，共 27 次 ADC。 */
  uint16_t v[3];
  for (uint8_t i = 0; i < 3; i++) {
    v[i] = readMedian(ch);
    if (i < 2) delay(20);
  }

  uint16_t a = v[0], b = v[1], c = v[2], t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
  (void)c;
  uint16_t med = b;

  if (med < ADC_OPEN_RAW)  { if (st) *st = FLT_OPEN;  return med; }
  if (med > ADC_SHORT_RAW) { if (st) *st = FLT_SHORT; return med; }
  if (st) *st = FLT_NONE;
  return med;
}

void sensorsApplyCalib(uint8_t ch, uint16_t dryRaw, uint16_t wetRaw) {
  if (ch >= CHANNEL_COUNT) return;
  ChannelCfg& c = storeCfgRW().ch[ch];

  /* 干点必须明显大于湿点，否则换算方向会反过来 */
  if (dryRaw <= wetRaw + 100) {
    if (dryRaw > wetRaw) {
      wetRaw = (uint16_t)(dryRaw - 100);
    } else {
      return;                        /* 两个值贴太近，宁可不改 */
    }
  }
  c.dryRaw = dryRaw;
  c.wetRaw = wetRaw;
  sensorsRecomputePct();
  logAdd(LOG_CALIB, (int8_t)ch);
  storeTouch();
}
