#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <string.h>
#include <stdarg.h>

#include "config.h"
#include "types.h"
#include "net.h"
#include "pump.h"
#include "scheduler.h"
#include "sensors.h"
#include "config_store.h"
#include "logbuf.h"
#include "jsonutil.h"

/* ================= 配置（NVS 命名空间 "mqtt"，与主配置互不干扰） ================= */
static Preferences s_pref;
static bool     s_en = false;
static char     s_host[MQTT_HOST_MAX_LEN + 1];
static uint16_t s_port = MQTT_PORT_DFLT;
static char     s_user[MQTT_USER_MAX_LEN + 1];
static char     s_pass[MQTT_PASS_MAX_LEN + 1];
static char     s_token[MQTT_TOKEN_MAX_LEN + 1];
static char     s_base[MQTT_BASE_MAX_LEN + 1];   /* 主题前缀，空则回落默认 */
static char     s_defBase[MQTT_BASE_MAX_LEN + 1];/* 由 MAC 推导的兜底前缀 */

/* ================= 运行时 ================= */
static WiFiClient     s_wc;
static PubSubClient   s_mqtt(s_wc);
static bool     s_conn = false;        /* 当前是否已连上 broker */
static uint32_t  s_nextConnMs = 0;     /* 下次尝试连接的时间戳（失败退避） */
static uint32_t  s_nextPubMs = 0;      /* 下次周期上报的时间戳 */
static bool      s_publishNow = false;  /* 有变更，立即上报 */
static bool      s_lastAct = false;     /* 上一拍泵是否在转（用于变更检测） */
static int8_t    s_lastCh = -1;
static uint8_t   s_lastFault = 0;       /* 故障掩码快照 */
static char      s_in[512];             /* 命令回调的临时缓冲 */

/* 远程页与局域网页功能对齐后，一次状态装不下定时任务和日志。
   采用「请求-应答」：仪表盘切到对应页时发 get 命令，设备把
   {"k":"t"} / {"k":"l"} 单独发到同一个 state 主题，避免为巴法云
   再预建主题（巴法云主题必须预先创建，且名字只允许字母数字）。
   位标记在回调里置位，回到 mqttTick 统一发布，避免在 callback 里
   递归调用 publish 或长时间阻塞 MQTT loop。 */
#define REPLY_STATE 1
#define REPLY_TASKS 2
#define REPLY_LOGS  4
static uint8_t  s_reply = 0;
static bool     s_rebootReq = false;

/* ================= 极简 JSON 拼接 ================= */
static int app2(char* b, int n, size_t cap, const char* fmt, ...) {
  if (n < 0) return 0;
  if ((size_t)n >= cap - 1) return (int)cap - 1;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(b + n, cap - (size_t)n, fmt, ap);
  va_end(ap);
  if (w < 0) return n;
  if ((size_t)(n + w) >= cap) return (int)cap - 1;
  return n + w;
}

/* ================= 主题名 =================
 * 巴法云(bemfa.com)不是标准层级主题 broker：
 *   1) 主题名只允许字母/数字，斜杠与下划线都会被拒(40009)；
 *   2) 主题必须先在控制台或 API 创建，否则订阅返回 0x80；
 *   3) "cmd" 后缀被平台保留，订阅同样被拒，改用 "ctrl"。
 * 因此巴法云下把 base 去掉非字母数字后拼接子主题，例如
 *   aw_AF48 -> awAF48state / awAF48avail / awAF48ctrl
 * 其他 broker(EMQX/自建)仍用标准层级主题 aw_AF48/state 等。 */
static bool bemfaMode(void) {
  return strstr(s_host, "bemfa.com") != nullptr;
}

static void topicName(char* out, size_t n, const char* sub) {
  if (!bemfaMode()) {
    snprintf(out, n, "%s/%s", s_base, sub);
    return;
  }
  char flat[MQTT_BASE_MAX_LEN + 1];
  size_t j = 0;
  for (size_t i = 0; s_base[i] && j < sizeof(flat) - 1; ++i) {
    unsigned char c = (unsigned char)s_base[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      flat[j++] = (char)c;
    }
  }
  flat[j] = 0;
  const char* s = (strcmp(sub, "cmd") == 0) ? "ctrl" : sub;
  snprintf(out, n, "%s%s", flat, s);
}

static void publishJson(const char* b) {
  char top[MQTT_BASE_MAX_LEN + 16];
  topicName(top, sizeof(top), "state");
  s_mqtt.publish(top, b, false);
}

/* ================= 状态上报 ================= */
static void publishState(void) {
  if (!s_conn) return;
  char b[MQTT_BUF_SIZE];
  int n = 0;
  uint32_t nowMs = millis();
  char ip[20];
  netIp(ip, sizeof(ip));

  n = app2(b, n, sizeof(b),
           "{\"k\":\"s\",\"fw\":\"%s\",\"up\":%lu,\"heap\":%lu,\"mode\":\"%s\",\"ip\":\"%s\","
           "\"rssi\":%d,\"samp\":%d,\"next\":%lu,\"cyc\":%d,",
           FW_VERSION,
           (unsigned long)(nowMs / 1000UL),
           (unsigned long)ESP.getFreeHeap(),
           (netMode() == NET_STA) ? "sta" : "ap",
           ip,
           netRssi(),
           sensorsSampling() ? 1 : 0,
           (unsigned long)(sensorsNextCycleMs(nowMs) / 1000UL),
           (int)storeSampleCycle());

  if (pumpIsActive()) {
    uint8_t src = pumpActiveSrc();
    const char* sn = (src == SRC_MANUAL) ? "manual" : (src == SRC_TIMER ? "timer" : "auto");
    n = app2(b, n, sizeof(b),
             "\"active\":{\"ch\":%d,\"src\":\"%s\",\"remain\":%lu},",
             (int)pumpActiveCh(), sn,
             (unsigned long)(pumpRemainMs() / 1000UL));
  } else {
    n = app2(b, n, sizeof(b), "\"active\":null,");
  }

  n = app2(b, n, sizeof(b), "\"ch\":[");
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    const ChannelCfg& c = storeCfg().ch[i];
    n = app2(b, n, sizeof(b),
             "%s{\"i\":%d,\"raw\":%d,\"pct\":%d,\"flt\":%d,\"auto\":%d,\"run\":%d,"
             "\"name\":\"%s\",\"thr\":%d,\"maxSec\":%d,\"gap\":%d,\"dry\":%d,\"wet\":%d}",
             (i ? "," : ""),
             (int)i,
             (int)sensorsRaw(i),
             (int)sensorsPct(i),
             (int)sensorsFault(i),
             (int)c.autoOn,
             (pumpIsActive() && pumpActiveCh() == (int8_t)i) ? 1 : 0,
             c.name,
             (int)c.thrPct,
             (int)c.maxSec,
             (int)c.minGapSec,
             (int)c.dryRaw,
             (int)c.wetRaw);
  }
  n = app2(b, n, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  publishJson(b);
}

/* 定时任务：单独一帧，仪表盘切到「定时」页时按需拉取 */
static void publishTasks(void) {
  if (!s_conn) return;
  char b[900];
  int n = 0;
  const ConfigBlob& cfg = storeCfg();
  n = app2(b, n, sizeof(b), "{\"k\":\"t\",\"tasks\":[");
  bool first = true;
  for (uint8_t i = 0; i < MAX_TASKS; i++) {
    const TaskCfg& t = cfg.tasks[i];
    if (!t.used) continue;
    n = app2(b, n, sizeof(b),
             "%s{\"i\":%d,\"on\":%d,\"ch\":%d,\"type\":%d,\"periodMin\":%d,"
             "\"durSec\":%d,\"hour\":%d,\"min\":%d}",
             first ? "" : ",", (int)i, (int)t.on, (int)t.ch, (int)t.type,
             (int)t.periodMin, (int)t.durSec, (int)t.hour, (int)t.minute);
    first = false;
  }
  n = app2(b, n, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  publishJson(b);
}

/* 最近 12 条运行日志，单独一帧 */
static void publishLogs(void) {
  if (!s_conn) return;
  static LogRec recs[12];
  uint32_t seq = logSeq();
  uint32_t since = (seq > 12) ? (seq - 12) : 0;
  uint16_t cnt = logSince(since, recs, 12);

  char b[1200];
  int n = 0;
  n = app2(b, n, sizeof(b), "{\"k\":\"l\",\"seq\":%lu,\"items\":[", (unsigned long)seq);
  for (uint16_t i = 0; i < cnt; i++) {
    n = app2(b, n, sizeof(b),
             "%s{\"s\":%lu,\"t\":%lu,\"c\":\"%s\",\"kind\":\"%s\",\"ch\":%d}",
             (i ? "," : ""),
             (unsigned long)recs[i].s,
             (unsigned long)recs[i].t,
             logText(recs[i].code),
             logKind(recs[i].code),
             (int)recs[i].ch);
  }
  n = app2(b, n, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  publishJson(b);
}

/* ================= 远程配置类命令 ================= */
/* 通道参数：与 /api/channel 同样的边界与 UTF-8 逐字复制保护 */
static void doSet(uint8_t ch, const String& body) {
  ConfigBlob& cfg = storeCfgRW();
  ChannelCfg& c = cfg.ch[ch];

  if (body.indexOf("\"auto\"") >= 0) {
    c.autoOn = jUtilBool(body, "auto", c.autoOn != 0) ? 1 : 0;
  }
  if (body.indexOf("\"thr\"") >= 0) {
    long v = jUtilInt(body, "thr", c.thrPct);
    if (v < 5) v = 5;
    if (v > 90) v = 90;
    c.thrPct = (uint8_t)v;
  }
  if (body.indexOf("\"maxSec\"") >= 0) {
    long v = jUtilInt(body, "maxSec", c.maxSec);
    if (v < 1) v = 1;
    if (v > HARD_MAX_SEC) v = HARD_MAX_SEC;
    c.maxSec = (uint16_t)v;
  }
  if (body.indexOf("\"gap\"") >= 0) {
    long v = jUtilInt(body, "gap", c.minGapSec);
    if (v < 0) v = 0;
    if (v > 7200) v = 7200;
    c.minGapSec = (uint16_t)v;
  }
  if (body.indexOf("\"name\"") >= 0) {
    char nm[CH_NAME_LEN];
    if (jUtilStr(body, "name", nm, sizeof(nm))) {
      char* d = c.name;
      const char* limit = c.name + sizeof(c.name) - 1;
      for (const char* s = nm; *s; ) {
        unsigned char uc = (unsigned char)*s;
        if (uc == '"' || uc == '\\') { s++; continue; }
        uint8_t len = 1;
        if (uc >= 0xF0)      len = 4;
        else if (uc >= 0xE0) len = 3;
        else if (uc >= 0xC0) len = 2;
        if (d + len > limit) break;
        for (uint8_t k = 0; k < len && s[k]; k++) *d++ = *s++;
      }
      *d = 0;
    }
  }
  storeTouch();
}

/* 标定取「最近一次采样的读数」，不重新驱动 ADC。
   MQTT 回调里做 6 路连采会阻塞 loop 数秒，容易被 broker 判超时；
   远程页提供「立即补采」按钮，先采样再标定即可拿到新鲜值。 */
static bool applyCal(uint8_t ch, bool isDry) {
  if (sensorsFault(ch)) return false;
  uint16_t r = sensorsRaw(ch);
  ConfigBlob& cfg = storeCfgRW();
  ChannelCfg& c = cfg.ch[ch];
  if (isDry) {
    if ((int32_t)r - (int32_t)c.wetRaw < 150) return false;
    c.dryRaw = r;
  } else {
    if ((int32_t)c.dryRaw - (int32_t)r < 150) return false;
    c.wetRaw = r;
  }
  logAdd(LOG_CALIB, (int8_t)ch);
  return true;
}

static void doCal(uint8_t ch, bool isDry) {
  if (applyCal(ch, isDry)) {
    sensorsRecomputePct();
    storeTouch();
    sensorsForceSample(ch);
  }
}

static void doCalAll(bool isDry) {
  if (pumpIsActive()) return;   /* 泵在转，读数不可信 */
  uint8_t okc = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (applyCal(i, isDry)) okc++;
  }
  if (okc) {
    sensorsRecomputePct();
    storeFlush();
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++) sensorsForceSample(i);
  }
}

/* 定时任务新增/修改：与 /api/schedule 同语义 */
static void doTask(const String& body) {
  ConfigBlob& cfg = storeCfgRW();
  long idx = jUtilInt(body, "i", -1);
  TaskCfg* t = nullptr;

  if (idx >= 0 && idx < MAX_TASKS && cfg.tasks[idx].used) {
    t = &cfg.tasks[idx];
  } else {
    for (uint8_t k = 0; k < MAX_TASKS; k++) {
      if (!cfg.tasks[k].used) {
        t = &cfg.tasks[k];
        memset(t, 0, sizeof(TaskCfg));
        t->used = 1;
        t->on = 1;
        t->ch = 0;
        t->type = 0;
        t->periodMin = 360;
        t->durSec = 15;
        idx = (long)k;
        break;
      }
    }
    if (!t) return;   /* 8 个槽位满了 */
  }

  if (body.indexOf("\"on\"") >= 0)   t->on = jUtilBool(body, "on", t->on != 0) ? 1 : 0;
  if (body.indexOf("\"ch\"") >= 0) {
    long v = jUtilInt(body, "ch", t->ch);
    if (v < 0) v = 0;
    if (v >= CHANNEL_COUNT) v = CHANNEL_COUNT - 1;
    t->ch = (uint8_t)v;
  }
  if (body.indexOf("\"type\"") >= 0) {
    long v = jUtilInt(body, "type", t->type);
    t->type = (v == 1) ? 1 : 0;
  }
  if (body.indexOf("\"periodMin\"") >= 0) {
    long v = jUtilInt(body, "periodMin", t->periodMin);
    if (v < 1) v = 1;
    if (v > 4320) v = 4320;
    t->periodMin = (uint16_t)v;
  }
  if (body.indexOf("\"durSec\"") >= 0) {
    long v = jUtilInt(body, "durSec", t->durSec);
    if (v < 1) v = 1;
    if (v > HARD_MAX_SEC) v = HARD_MAX_SEC;
    t->durSec = (uint16_t)v;
  }
  if (body.indexOf("\"hour\"") >= 0) {
    long v = jUtilInt(body, "hour", t->hour);
    if (v < 0) v = 0;
    if (v > 23) v = 23;
    t->hour = (uint8_t)v;
  }
  if (body.indexOf("\"min\"") >= 0) {
    long v = jUtilInt(body, "min", t->minute);
    if (v < 0) v = 0;
    if (v > 59) v = 59;
    t->minute = (uint8_t)v;
  }
  t->lastRunSec = millis() / 1000UL;
  storeTouch();
}

static void doTaskDel(const String& body) {
  long idx = jUtilInt(body, "i", -1);
  if (idx < 0 || idx >= MAX_TASKS) return;
  memset(&storeCfgRW().tasks[idx], 0, sizeof(TaskCfg));
  storeTouch();
}

/* ================= 命令回调 ================= */
static void onMessage(char* topic, byte* payload, unsigned int len) {
  (void)topic;
  if (len == 0 || len >= sizeof(s_in)) return;
  memcpy(s_in, payload, len);
  s_in[len] = 0;
  String body = String((const char*)s_in);

  /* 令牌校验：配置了 token 就必须带对，否则整条丢弃。 */
  char tk[MQTT_TOKEN_MAX_LEN + 1];
  bool hasTok = jUtilStr(body, "t", tk, sizeof(tk));
  if (s_token[0] != 0) {
    if (!hasTok || strncmp(tk, s_token, MQTT_TOKEN_MAX_LEN) != 0) return;
  }

  char act[12];
  if (!jUtilStr(body, "a", act, sizeof(act))) strcpy(act, "start");
  char what[8] = {0};
  long ch = jUtilInt(body, "ch", -1);

  /* --- 不需要通道号的全局动作 --- */
  if (strcmp(act, "get") == 0) {
    if (jUtilStr(body, "w", what, sizeof(what))) {
      if (strcmp(what, "tasks") == 0)      s_reply |= REPLY_TASKS;
      else if (strcmp(what, "logs") == 0)  s_reply |= REPLY_LOGS;
      else if (strcmp(what, "all") == 0)   s_reply |= REPLY_STATE | REPLY_TASKS | REPLY_LOGS;
      else                                 s_reply |= REPLY_STATE;
    } else {
      s_reply |= REPLY_STATE;
    }
    return;
  }
  if (strcmp(act, "task") == 0)    { doTask(body);    s_reply |= REPLY_TASKS | REPLY_STATE; return; }
  if (strcmp(act, "taskdel") == 0) { doTaskDel(body);  s_reply |= REPLY_TASKS; return; }
  if (strcmp(act, "calall") == 0) {
    bool dry = !jUtilBool(body, "wet", false);
    doCalAll(dry);
    s_reply |= REPLY_STATE | REPLY_LOGS;
    return;
  }
  if (strcmp(act, "cycle") == 0) {
    long v = jUtilInt(body, "min", 0);
    if (v > 0) storeSampleCycleSet((uint16_t)v);
    s_reply |= REPLY_STATE;
    return;
  }
  if (strcmp(act, "sample") == 0) { sensorsRequestNow(); s_reply |= REPLY_STATE; return; }
  if (strcmp(act, "reboot") == 0) { s_rebootReq = true; return; }

  /* --- 以下动作必须带合法通道号 --- */
  if (ch < 0 || ch >= CHANNEL_COUNT) return;

  if (strcmp(act, "stop") == 0) {
    if (pumpIsActive() && pumpActiveCh() == (int8_t)ch) pumpStop(STOP_MANUAL);
    else schedCancel((uint8_t)ch);
    s_reply |= REPLY_STATE;
    return;
  }
  if (strcmp(act, "auto") == 0) {
    ConfigBlob& cfg = storeCfgRW();
    cfg.ch[ch].autoOn = jUtilBool(body, "on", true) ? 1 : 0;
    storeTouch();
    s_reply |= REPLY_STATE;
    return;
  }
  if (strcmp(act, "set") == 0) {
    doSet((uint8_t)ch, body);
    s_reply |= REPLY_STATE;
    return;
  }
  if (strcmp(act, "cal") == 0) {
    doCal((uint8_t)ch, !jUtilBool(body, "wet", false));
    s_reply |= REPLY_STATE | REPLY_LOGS;
    return;
  }

  /* 默认 action = start */
  long sec = jUtilInt(body, "s", storeCfg().ch[ch].maxSec);
  if (sec < 1) sec = 1;
  if (sec > HARD_MAX_SEC) sec = HARD_MAX_SEC;
  if (schedRequest((uint8_t)ch, (uint32_t)sec * 1000UL, SRC_MANUAL) == REQ_OK) {
    s_reply |= REPLY_STATE;
  }
}

/* ================= 连接尝试 ================= */
static void attemptConnect(uint32_t nowMs) {
  s_wc.setTimeout(3000);
  s_mqtt.setServer(s_host, s_port);

  char cid[MQTT_BASE_MAX_LEN + 8];
  /* 巴法云按 clientId = uid 路由消息：user 字段填了 uid 就用作 clientId；
     通用 broker（EMQX/自建）回落到主题前缀，避免空 clientId。 */
  snprintf(cid, sizeof(cid), "%s", s_user[0] ? s_user : s_base);

  char avail[MQTT_BASE_MAX_LEN + 16];
  topicName(avail, sizeof(avail), "avail");

  bool ok = s_mqtt.connect(cid,
                           s_user[0]  ? s_user  : nullptr,
                           s_pass[0]  ? s_pass  : nullptr,
                           avail, 0, true, "offline");

  if (!ok) {
    s_conn = false;
    s_nextConnMs = nowMs + MQTT_RETRY_MS;
    return;
  }

  s_conn = true;
  char cmd[MQTT_BASE_MAX_LEN + 16];
  topicName(cmd, sizeof(cmd), "cmd");
  s_mqtt.subscribe(cmd);
  s_mqtt.publish(avail, "online", true);
  publishState();
  s_nextPubMs = nowMs + MQTT_PUB_MS;
  s_lastAct = pumpIsActive();
  s_lastCh  = s_lastAct ? pumpActiveCh() : -1;
  logAdd(LOG_MQTT, -1);
}

/* ================= 生命周期 ================= */
void mqttBegin(void) {
  uint32_t mac = (uint32_t)(ESP.getEfuseMac() & 0xFFFF);
  snprintf(s_defBase, sizeof(s_defBase), "aw_%04X", (unsigned)mac);

  s_en = false;
  s_host[0] = 0;
  s_port = MQTT_PORT_DFLT;
  s_user[0] = 0;
  s_pass[0] = 0;
  s_token[0] = 0;
  s_base[0] = 0;

  if (s_pref.begin("mqtt", false)) {
    s_en   = s_pref.getBool("en", false);
    s_pref.getString("host", s_host, sizeof(s_host));
    s_port = s_pref.getUShort("port", MQTT_PORT_DFLT);
    if (s_port == 0) s_port = MQTT_PORT_DFLT;
    s_pref.getString("user", s_user, sizeof(s_user));
    s_pref.getString("pass", s_pass, sizeof(s_pass));
    s_pref.getString("token", s_token, sizeof(s_token));
    s_pref.getString("base", s_base, sizeof(s_base));
    s_pref.end();
  }

  if (!s_base[0]) {
    strncpy(s_base, s_defBase, sizeof(s_base) - 1);
    s_base[sizeof(s_base) - 1] = 0;
  }

  s_mqtt.setBufferSize(MQTT_BUF_SIZE);
  s_mqtt.setCallback(onMessage);
  s_conn = false;
  s_nextConnMs = 0;
  s_lastAct = false;
  s_lastCh = -1;
  s_lastFault = 0;
  s_reply = 0;
  s_rebootReq = false;
}

void mqttTick(uint32_t nowMs) {
  if (s_rebootReq) { delay(80); ESP.restart(); }

  if (!s_en || s_host[0] == 0) {
    if (s_conn) { s_mqtt.disconnect(); s_conn = false; }
    return;
  }
  if (netMode() != NET_STA) {
    if (s_conn) { s_mqtt.disconnect(); s_conn = false; }
    return;
  }

  if (!s_conn) {
    if ((int32_t)(nowMs - s_nextConnMs) < 0) return;
    attemptConnect(nowMs);
    return;
  }

  s_mqtt.loop();

  bool act = pumpIsActive();
  int8_t ch = act ? pumpActiveCh() : -1;
  if (act != s_lastAct || ch != s_lastCh) {
    s_lastAct = act;
    s_lastCh = ch;
    s_publishNow = true;
  }
  uint8_t fm = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (sensorsFault(i)) fm |= (uint8_t)(1u << i);
  }
  if (fm != s_lastFault) {
    s_lastFault = fm;
    s_publishNow = true;
  }
  if ((int32_t)(nowMs - s_nextPubMs) >= 0) {
    s_nextPubMs = nowMs + MQTT_PUB_MS;
    s_publishNow = true;
  }

  /* 回调里挂起的应答统一在这里发；连发多帧也不会重入 publish */
  if (s_reply) {
    uint8_t r = s_reply;
    s_reply = 0;
    if (r & REPLY_STATE) publishState();
    if (r & REPLY_TASKS) publishTasks();
    if (r & REPLY_LOGS)  publishLogs();
    s_nextPubMs = nowMs + MQTT_PUB_MS;
    s_publishNow = false;
  } else if (s_publishNow) {
    s_publishNow = false;
    publishState();
  }
}

void mqttPublishState(void) { s_publishNow = true; }

bool mqttEnabled(void)   { return s_en; }
bool mqttConnected(void) { return s_conn; }

/* ================= 配置读写 ================= */
bool mqttCfgEnabled(void) { return s_en; }
void mqttCfgEnabledSet(bool on) {
  s_en = on;
  if (s_pref.begin("mqtt", false)) { s_pref.putBool("en", on); s_pref.end(); }
}

const char* mqttCfgHost(void) { return s_host; }
void mqttCfgHostSet(const char* h) {
  if (!h) return;
  strncpy(s_host, h, MQTT_HOST_MAX_LEN);
  s_host[MQTT_HOST_MAX_LEN] = 0;
  if (s_pref.begin("mqtt", false)) { s_pref.putString("host", s_host); s_pref.end(); }
  s_nextConnMs = 0;
}

uint16_t mqttCfgPort(void) { return s_port; }
void mqttCfgPortSet(uint16_t p) {
  if (p == 0) p = MQTT_PORT_DFLT;
  s_port = p;
  if (s_pref.begin("mqtt", false)) { s_pref.putUShort("port", p); s_pref.end(); }
  s_nextConnMs = 0;
}

const char* mqttCfgUser(void) { return s_user; }
void mqttCfgUserSet(const char* u) {
  if (!u) return;
  strncpy(s_user, u, MQTT_USER_MAX_LEN);
  s_user[MQTT_USER_MAX_LEN] = 0;
  if (s_pref.begin("mqtt", false)) { s_pref.putString("user", s_user); s_pref.end(); }
  s_nextConnMs = 0;
}

const char* mqttCfgPass(void) { return s_pass; }
void mqttCfgPassSet(const char* p) {
  if (!p) return;
  strncpy(s_pass, p, MQTT_PASS_MAX_LEN);
  s_pass[MQTT_PASS_MAX_LEN] = 0;
  if (s_pref.begin("mqtt", false)) { s_pref.putString("pass", s_pass); s_pref.end(); }
  s_nextConnMs = 0;
}

const char* mqttCfgToken(void) { return s_token; }
void mqttCfgTokenSet(const char* t) {
  if (!t) return;
  strncpy(s_token, t, MQTT_TOKEN_MAX_LEN);
  s_token[MQTT_TOKEN_MAX_LEN] = 0;
  if (s_pref.begin("mqtt", false)) { s_pref.putString("token", s_token); s_pref.end(); }
}

const char* mqttCfgBase(void) { return s_base; }
void mqttCfgBaseSet(const char* b) {
  if (!b) return;
  char tmp[MQTT_BASE_MAX_LEN + 1];
  strncpy(tmp, b, MQTT_BASE_MAX_LEN);
  tmp[MQTT_BASE_MAX_LEN] = 0;
  if (tmp[0] == 0) {
    strncpy(s_base, s_defBase, sizeof(s_base) - 1);
    s_base[sizeof(s_base) - 1] = 0;
  } else {
    strncpy(s_base, tmp, sizeof(s_base) - 1);
    s_base[sizeof(s_base) - 1] = 0;
  }
  if (s_pref.begin("mqtt", false)) { s_pref.putString("base", s_base); s_pref.end(); }
  s_nextConnMs = 0;
}
