#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "types.h"
#include "config_store.h"
#include "logbuf.h"

/* NVS 命名空间名不得超过 15 字符，否则会被静默截断 */
static const char* NS_CFG = "water";
static const char* NS_SYS = "watersys";

static Preferences s_pref;
static ConfigBlob  s_cfg;
static bool        s_dirty = false;
static uint32_t    s_dirtyAt = 0;
static char        s_apPass[AP_PASS_MAX_LEN + 1];
static uint16_t    s_cycleMin = DEF_SAMPLE_CYCLE_MIN;

/* ---------- CRC32（多项式 0xEDB88320，逐位实现，省一张表） ---------- */
uint32_t storeCrc(const ConfigBlob& c) {
  const uint8_t* p = (const uint8_t*)&c;
  size_t len = sizeof(ConfigBlob) - sizeof(uint32_t);   /* 不含 crc 自身 */
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (uint8_t k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

void storeDefaults(ConfigBlob& c) {
  memset(&c, 0, sizeof(c));
  c.ver = CFG_VERSION;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    c.ch[i].autoOn    = DEF_AUTO_ON;
    c.ch[i].type      = SENS_HUMID;     /* 出厂按湿度探头算，pH 通道由网页改 */
    c.ch[i].thrPct    = DEF_THR_PCT;
    c.ch[i].maxSec    = DEF_MAX_SEC;
    c.ch[i].minGapSec = DEF_MIN_GAP_SEC;
    c.ch[i].dryRaw    = DEF_DRY_RAW;
    c.ch[i].wetRaw    = DEF_WET_RAW;
  }
  for (uint8_t i = 0; i < MAX_TASKS; i++) c.tasks[i].used = 0;
  c.ssid[0] = 0;
  c.pass[0] = 0;
  c.crc = storeCrc(c);
}

void storeBegin(void) {
  storeDefaults(s_cfg);

  /* AP 密码先兜底：万一下面 NVS 打不开，也不能退化成开放热点。
     开放热点意味着任何人连上就能开泵，比"密码没生效"危险得多。 */
  strncpy(s_apPass, DEF_AP_PASS, sizeof(s_apPass) - 1);
  s_apPass[sizeof(s_apPass) - 1] = 0;

  /* 采样周期同理先给默认值：读到 0 会让"距上次采样已超周期"恒成立，
     等于每个 tick 都开一轮采样，把系统直接拖死。 */
  s_cycleMin = DEF_SAMPLE_CYCLE_MIN;

  if (s_pref.begin(NS_CFG, false)) {
    size_t got = s_pref.getBytesLength("cfg");
    if (got == sizeof(ConfigBlob)) {
      ConfigBlob tmp;
      s_pref.getBytes("cfg", &tmp, sizeof(tmp));
      if (tmp.ver == CFG_VERSION && tmp.crc == storeCrc(tmp)) {
        s_cfg = tmp;
      } else {
        logAdd(LOG_CFG_BAD, -1);
      }
    }
    /* 首次上电没有 blob 属正常，不报错 */

    /* AP 密码：独立 key，与主配置互不影响。
       读出来短于下限（没存过 / 存了坏值）就回落到出厂默认并立刻写回，
       避免把非法密码交给 softAP() 导致热点根本起不来。 */
    if (s_pref.getString("appass", s_apPass, sizeof(s_apPass)) < (size_t)AP_PASS_MIN_LEN) {
      strncpy(s_apPass, DEF_AP_PASS, sizeof(s_apPass) - 1);
      s_apPass[sizeof(s_apPass) - 1] = 0;
      s_pref.putString("appass", s_apPass);
    }

    /* 采样周期：独立 key，越界或缺失都回落到出厂值 */
    uint16_t cy = s_pref.getUShort("cycle", DEF_SAMPLE_CYCLE_MIN);
    if (cy >= SAMPLE_CYCLE_MIN && cy <= SAMPLE_CYCLE_MAX) {
      s_cycleMin = cy;
    } else {
      s_cycleMin = DEF_SAMPLE_CYCLE_MIN;
      s_pref.putUShort("cycle", s_cycleMin);
    }

    s_pref.end();
  }
}

const ConfigBlob& storeCfg(void) { return s_cfg; }
ConfigBlob&       storeCfgRW(void) { return s_cfg; }

void storeTouch(void) {
  s_dirty = true;
  s_dirtyAt = millis();
}

void storeFlush(void) {
  s_cfg.ver = CFG_VERSION;
  s_cfg.crc = storeCrc(s_cfg);
  if (s_pref.begin(NS_CFG, false)) {
    s_pref.putBytes("cfg", &s_cfg, sizeof(ConfigBlob));
    s_pref.end();
  }
  s_dirty = false;
  logAdd(LOG_CFG_SAVE, -1);
}

void storeTick(uint32_t nowMs) {
  if (s_dirty && (nowMs - s_dirtyAt) >= CFG_SAVE_DEBOUNCE_MS) {
    storeFlush();
  }
}

/* ---------- 热点密码 ---------- */
const char* storeApPass(void) { return s_apPass; }

void storeApPassSet(const char* p) {
  if (!p) return;
  size_t n = strlen(p);
  /* 越界一律拒绝：太短 softAP() 会失败、热点起不来，太长会截断成别的密码。
     校验放在这里而不是调用方，是因为将来可能有第二条修改路径。 */
  if (n < AP_PASS_MIN_LEN || n > AP_PASS_MAX_LEN) return;

  memset(s_apPass, 0, sizeof(s_apPass));
  strncpy(s_apPass, p, sizeof(s_apPass) - 1);

  if (s_pref.begin(NS_CFG, false)) {
    s_pref.putString("appass", s_apPass);
    s_pref.end();
  }
}

/* ---------- 采样周期 ---------- */
uint16_t storeSampleCycle(void) { return s_cycleMin; }

void storeSampleCycleSet(uint16_t min) {
  /* 越界直接忽略：0 会让"距上次采样已超周期"恒成立，采样循环失控；
     太长又会一直拿着过期读数做判断 */
  if (min < SAMPLE_CYCLE_MIN || min > SAMPLE_CYCLE_MAX) return;
  s_cycleMin = min;
  if (s_pref.begin(NS_CFG, false)) {
    s_pref.putUShort("cycle", min);
    s_pref.end();
  }
}

/* ---------- 系统标志 ---------- */
void storeRunMarkerSet(bool on) {
  if (s_pref.begin(NS_SYS, false)) {
    s_pref.putBool("run", on);
    s_pref.end();
  }
}

bool storeRunMarkerGet(void) {
  bool v = false;
  if (s_pref.begin(NS_SYS, false)) {
    v = s_pref.getBool("run", false);
    s_pref.end();
  }
  return v;
}

uint8_t storeBadBootCount(void) {
  uint8_t v = 0;
  if (s_pref.begin(NS_SYS, false)) {
    v = s_pref.getUChar("bad", 0);
    s_pref.end();
  }
  return v;
}

void storeBadBootCountSet(uint8_t n) {
  if (s_pref.begin(NS_SYS, false)) {
    s_pref.putUChar("bad", n);
    s_pref.end();
  }
}
