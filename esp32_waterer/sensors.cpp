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

static uint8_t  s_rr = 0;                    /* 轮询游标（立即采样时用） */
static uint16_t s_lastGood[CHANNEL_COUNT];   /* 0xFFFF = 还没有有效读数 */

/* ---------- 探头供电周期状态机 ----------
 * IDLE    断电待机，等下一个 30 分钟周期
 * SETTLE  刚上电，等探头输出稳定
 * READING 正在逐路采样，采完立刻断电
 */
enum : uint8_t { SP_IDLE = 0, SP_SETTLE, SP_READING };

static uint8_t  s_pwrState = SP_IDLE;
static uint32_t s_pwrAtMs = 0;        /* 进入当前状态的时刻 */
static uint32_t s_pwrOnAtMs = 0;      /* 本次上电时刻，0 = 当前断电 */
static uint32_t s_lastCycleMs = 0;    /* 上一轮采完的时刻 */
static uint8_t  s_readCh = 0;         /* 本轮正在采哪一路 */
static uint8_t  s_repIdx = 0;         /* 该路已经采了几遍 */
static bool     s_bootDone = false;   /* 首轮是否已完成：没采过就立刻采，不等 30 分钟 */
static bool     s_reqNow = false;     /* 有人点名要立刻补采一轮 */
static bool     s_pwrFault = false;   /* 探头供电异常（六路同时贴地），全局状态 */

/* 采样周期由网页设置、存 NVS，这里换算成毫秒 */
static uint32_t cycleMs(void) {
  return (uint32_t)storeSampleCycle() * 60000UL;
}

/* ============ 探头供电开关 ============ */
static void sensorPowerOn(void) {
  /* 先定电平再切输出：pinMode 会立刻让引脚按输出寄存器当前值驱动，
     顺序反了会先输出旧电平再纠正，产生一个窄毛刺。 */
  digitalWrite(PIN_SENSOR_PWR, SENSOR_PWR_ON_LEVEL);
  pinMode(PIN_SENSOR_PWR, OUTPUT);
  digitalWrite(PIN_SENSOR_PWR, SENSOR_PWR_ON_LEVEL);
  s_pwrOnAtMs = millis();
}

static void sensorPowerOff(void) {
  digitalWrite(PIN_SENSOR_PWR, SENSOR_PWR_OFF_LEVEL);
  pinMode(PIN_SENSOR_PWR, OUTPUT);
  digitalWrite(PIN_SENSOR_PWR, SENSOR_PWR_OFF_LEVEL);
  s_pwrOnAtMs = 0;
}

/* 等探头输出稳定。已经稳了就直接返回 ——
   一键标定要连读 6 路，每路都干等 500ms 会卡三秒。 */
static void sensorWaitStable(void) {
  if (s_pwrOnAtMs == 0) return;
  uint32_t el = (uint32_t)(millis() - s_pwrOnAtMs);
  if (el < SENSOR_SETTLE_MS) delay(SENSOR_SETTLE_MS - el);
}

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

/* ============ 探头供电检测 ============
 * 六路读数同时贴地，几乎不可能是六个探头约好了一起坏，
 * 而是 VCC 根本没上来：继电器没吸合、线圈断、前级掉电。
 * 这时若按每路断线去报，你会看到六个独立的"传感器故障"，
 * 误以为探头全废，实际该查的是供电。 */
static bool pwrAllDead(void) {
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (readMedian(i) >= ADC_OPEN_RAW) return false;   /* 有一路活着就说明供电正常 */
  }
  return true;
}

static void setPwrFault(bool on) {
  if (s_pwrFault == on) return;
  s_pwrFault = on;
  /* 直接写 s_fault，不走 raiseFault：后者每路都会刷一条日志，
     六路一起刷反而把"供电"这个根因埋掉了。 */
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (on) {
      s_fault[i] = FLT_PWR;                    /* 盖掉各路原有状态 */
    } else if (s_fault[i] == FLT_PWR) {
      s_fault[i] = FLT_NONE;                   /* 只清供电码，其它故障留给各自判定 */
    }
  }
  logAdd(on ? LOG_PWR_FAIL : LOG_PWR_OK, -1);
}

bool sensorsPwrFault(void) { return s_pwrFault; }

/* ============ 单路采样 + 质量判定 ============ */
static void sampleChannel(uint8_t ch, uint32_t nowMs) {
  uint16_t raw = readMedian(ch);

  /* ---- 1. 断线 / 短路（必须最优先）----
   * 0 和满量程是明确的电气状态，不该先去过抖：间歇供电下一轮每路只采 3 遍，
   * 若排在被跳变过滤后面，探头拔掉要等好几个周期才认得出，
   * 而这段时间 pct 已经是 0，自动浇水会以为土干而盲目开泵。
   * 100k 泄放电阻保证探头拔掉时读数落到 0 附近，
   * 因此 raw ≈ 0 能和"干土"区分开（干土在 2800~3300）。 */
  if (raw < ADC_OPEN_RAW) {
    s_raw[ch] = raw;
    s_ready[ch] = true;
    s_pct[ch] = 0;
    s_goodCnt[ch] = 0;
    s_sameCnt[ch] = 0;
    s_rejStreak[ch] = 0;
    s_lastGood[ch] = 0xFFFF;              /* 作废基准，恢复后不做跳变比对 */
    if (s_badCnt[ch] < 255) s_badCnt[ch]++;
    if (s_badCnt[ch] >= FAULT_CONFIRM_COUNT) raiseFault(ch, FLT_OPEN);
    return;
  }
  if (raw > ADC_SHORT_RAW) {
    s_raw[ch] = raw;
    s_ready[ch] = true;
    s_pct[ch] = 0;
    s_goodCnt[ch] = 0;
    s_sameCnt[ch] = 0;
    s_rejStreak[ch] = 0;
    s_lastGood[ch] = 0xFFFF;
    if (s_badCnt[ch] < 255) s_badCnt[ch]++;
    if (s_badCnt[ch] >= FAULT_CONFIRM_COUNT) raiseFault(ch, FLT_SHORT);
    return;
  }

  /* ---- 2. 单次跳变过滤 ----
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

  /* 探头先断电：上电第一件事就是断，避免传感器带着电进入初始化 */
  sensorPowerOff();
  s_pwrState = SP_IDLE;
  s_pwrAtMs = 0;
  s_lastCycleMs = 0;
  s_readCh = 0;
  s_repIdx = 0;
  s_bootDone = false;
  s_reqNow = false;

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
  /* 间歇供电下探头平时不带电，直接读只会得到 0，
     反而会把通道误判成断线、顺手锁死水泵。
     所以这里不立即读，只登记一次"尽快补采"的请求。 */
  (void)ch;
  sensorsRequestNow();
}

void sensorsTick(uint32_t nowMs, bool pumpRecent) {
  /* 泵启停后的地弹窗口内什么都不做：不但不能采样，也不能在这时候上电，
     否则这一轮读数全是脏的，还会污染后续的故障判定。 */
  if (pumpRecent) return;

  switch (s_pwrState) {
    case SP_IDLE:
      /* 首轮、到周期、或被点名补采，就开一轮 */
      if (!s_bootDone || s_reqNow ||
          (uint32_t)(nowMs - s_lastCycleMs) >= cycleMs()) {
        sensorPowerOn();
        s_pwrAtMs = nowMs;
        s_readCh = 0;
        s_repIdx = 0;
        s_pwrState = SP_SETTLE;
      }
      break;

    case SP_SETTLE:
      /* 探头内部有稳压和滤波电容，刚上电的读数是漂的 */
      if ((uint32_t)(nowMs - s_pwrAtMs) >= SENSOR_SETTLE_MS) {
        /* 上电确认：电要是没上去，再采 400ms 也只是六份垃圾，
           还会把六路都判成断线。先探一下，死了就直接收工。 */
        if (pwrAllDead()) {
          setPwrFault(true);
          sensorPowerOff();
          s_pwrState = SP_IDLE;
          s_lastCycleMs = nowMs;
          s_bootDone = true;
          s_reqNow = false;
        } else {
          setPwrFault(false);
          s_pwrAtMs = nowMs;
          s_pwrState = SP_READING;
        }
      }
      break;

    case SP_READING:
      if ((uint32_t)(nowMs - s_pwrAtMs) >= SENSOR_READ_GAP_MS) {
        s_pwrAtMs = nowMs;
        sampleChannel(s_readCh, nowMs);

        if (++s_repIdx >= SENSOR_REPEAT) {
          s_repIdx = 0;
          if (++s_readCh >= CHANNEL_COUNT) {
            /* 六路采完立刻断电：带电时间越短，极板电化学腐蚀越慢。
               一轮实际带电约 500 + 6×3×20 = 860ms，占 30 分钟周期的 0.05%。 */
            sensorPowerOff();
            s_pwrState = SP_IDLE;
            s_lastCycleMs = nowMs;
            s_bootDone = true;
            s_reqNow = false;
          }
        }
      }
      break;
  }
}

/* 请求立刻补采一轮。浇水结束后用：土湿已经变了，30 分钟后才知道太迟，
   中间自动逻辑会拿着"浇水前"的旧读数继续判断。 */
void sensorsRequestNow(void) {
  /* 离上一轮太近就别重复开：一轮本身要耗约 860ms 的电，
     频繁手动浇水时每次停泵都补采，省电的意义就没了。 */
  if (s_bootDone && (uint32_t)(millis() - s_lastCycleMs) < 60000UL) return;
  s_reqNow = true;
}

bool sensorsSampling(void) {
  return s_pwrState != SP_IDLE;
}

uint32_t sensorsNextCycleMs(uint32_t nowMs) {
  if (s_pwrState != SP_IDLE || !s_bootDone) return 0;
  uint32_t cyc = cycleMs();
  uint32_t el = (uint32_t)(nowMs - s_lastCycleMs);
  return (el >= cyc) ? 0 : (cyc - el);
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

  /* 标定要的是"此刻的真实读数"。间歇供电下探头平时是断电的，
     直接读只会拿到 0，所以这里自己把电加上、等稳了再采，
     采完恢复原来的供电状态。会阻塞约 500ms，但标定是用户主动点的，可以接受。 */
  bool wasOn = (s_pwrState != SP_IDLE);
  if (!wasOn) sensorPowerOn();
  sensorWaitStable();

  /* 单次 9 采样容易被 WiFi 发射尖峰带偏，标定要写进 NVS、以后一直用，
     所以多采两组再取一次中位数，共 27 次 ADC。 */
  uint16_t v[3];
  for (uint8_t i = 0; i < 3; i++) {
    v[i] = readMedian(ch);
    if (i < 2) delay(20);
  }

  /* 数据已经拿到，先断电再走后面的判断：
     下面有三个 return 分支，放到后面断电会漏掉 */
  if (!wasOn) sensorPowerOff();

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
