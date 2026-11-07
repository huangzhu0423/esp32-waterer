#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>
#include <stdarg.h>

#include "config.h"
#include "types.h"
#include "web_server.h"
#include "web_ui.h"
#include "sensors.h"
#include "pump.h"
#include "scheduler.h"
#include "config_store.h"
#include "health.h"
#include "logbuf.h"
#include "net.h"

static WebServer s_http(80);
static uint32_t  s_rebootAt = 0;
static uint32_t  s_apRestartAt = 0;   /* 改密后延迟重开热点，先让 HTTP 响应发出去 */

/* ================= JSON 输出小工具 =================
 * 全部用栈缓冲 + vsnprintf，不用 String 拼接，
 * 因为 ESP32 上堆碎片是长时间运行崩溃的头号原因。 */
static int app(char* b, int n, size_t cap, const char* fmt, ...) {
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

/* ================= 极简 JSON 取值 ================= */
static bool jPos(const String& b, const char* key, int& p) {
  String pat = "\"";
  pat += key;
  pat += "\":";
  int i = b.indexOf(pat);
  if (i < 0) return false;
  p = i + (int)pat.length();
  return true;
}

static long jInt(const String& b, const char* key, long def) {
  int p;
  if (!jPos(b, key, p)) return def;
  int len = (int)b.length();
  while (p < len && (b[p] == ' ' || b[p] == '\t')) p++;
  bool neg = false;
  if (p < len && b[p] == '-') { neg = true; p++; }
  long v = 0;
  bool any = false;
  while (p < len && b[p] >= '0' && b[p] <= '9') {
    v = v * 10 + (b[p] - '0');
    p++;
    any = true;
  }
  if (!any) return def;
  return neg ? -v : v;
}

static bool jBool(const String& b, const char* key, bool def) {
  String t = "\"";
  t += key;
  t += "\":true";
  if (b.indexOf(t) >= 0) return true;
  String f = "\"";
  f += key;
  f += "\":false";
  if (b.indexOf(f) >= 0) return false;
  return jInt(b, key, def ? 1 : 0) != 0;
}

static bool jStr(const String& b, const char* key, char* out, size_t n) {
  String pat = "\"";
  pat += key;
  pat += "\":\"";
  int i = b.indexOf(pat);
  if (i < 0) return false;
  int s = i + (int)pat.length();
  int e = b.indexOf('"', s);
  if (e < 0) return false;
  String v = b.substring(s, e);
  strncpy(out, v.c_str(), n - 1);
  out[n - 1] = 0;
  return true;
}

static const char* errText(uint8_t r) {
  switch (r) {
    case REQ_ERR_FAULT:    return "sensor_fault";
    case REQ_ERR_BUDGET:   return "budget";
    case REQ_ERR_BUSY:     return "busy";
    case REQ_ERR_COOLDOWN: return "cooldown";
    case REQ_ERR_LOCKOUT:  return "lockout";
    case REQ_ERR_INVALID:  return "invalid";
    default:               return "ok";
  }
}

static void sendJson(const char* body) {
  s_http.sendHeader("Cache-Control", "no-store");
  s_http.send(200, "application/json", body);
}

static void sendErr(uint8_t result) {
  char b[96];
  snprintf(b, sizeof(b), "{\"ok\":0,\"err\":\"%s\"}", errText(result));
  sendJson(b);
}

/* ================= 首页 ================= */
static void handleRoot(void) {
  s_http.sendHeader("Content-Encoding", "gzip");
  s_http.sendHeader("Cache-Control", "no-store");
  s_http.send_P(200, "text/html", (const char*)WEB_UI_GZ, WEB_UI_GZ_LEN);
}

/* ================= 状态快照 ================= */
static void handleState(void) {
  /* 2048 而不是 1200：给后续 32 路和 pH 字段留余量。
     这个缓冲在栈上，ESP32 默认任务栈 8KB，2KB 还在安全范围内。 */
  char b[2048];
  int n = 0;
  uint32_t nowMs = millis();
  char ip[20];
  netIp(ip, sizeof(ip));

  n = app(b, n, sizeof(b),
          "{\"ok\":1,\"fw\":\"%s\",\"up\":%lu,\"heap\":%lu,\"mode\":\"%s\",\"ip\":\"%s\","
          "\"rssi\":%d,\"timeOk\":%d,\"q\":%d,\"lock\":%lu,\"samp\":%d,\"next\":%lu,",
          FW_VERSION,
          (unsigned long)(nowMs / 1000UL),
          (unsigned long)ESP.getFreeHeap(),
          (netMode() == NET_STA) ? "sta" : "ap",
          ip,
          netRssi(),
          netTimeSynced() ? 1 : 0,
          (int)schedQueueLen(),
          (unsigned long)healthLockRemainSec(nowMs),
          sensorsSampling() ? 1 : 0,
          (unsigned long)(sensorsNextCycleMs(nowMs) / 1000UL));

  if (pumpIsActive()) {
    uint8_t src = pumpActiveSrc();
    const char* sn = (src == SRC_MANUAL) ? "manual" : (src == SRC_TIMER ? "timer" : "auto");
    n = app(b, n, sizeof(b),
            "\"active\":{\"ch\":%d,\"src\":\"%s\",\"remain\":%lu},",
            (int)pumpActiveCh(), sn,
            (unsigned long)(pumpRemainMs() / 1000UL));
  } else {
    n = app(b, n, sizeof(b), "\"active\":null,");
  }

  n = app(b, n, sizeof(b), "\"ch\":[");
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    const ChannelCfg& c = storeCfg().ch[i];
    n = app(b, n, sizeof(b),
            "%s{\"i\":%d,\"type\":%d,\"name\":\"%s\",\"raw\":%d,\"pct\":%d,\"flt\":%d,"
            "\"auto\":%d,\"thr\":%d,\"run\":%d,\"q\":%d,\"bud\":%lu,\"dry\":%d,\"wet\":%d}",
            (i ? "," : ""),
            (int)i,
            (int)c.type,
            c.name,
            (int)sensorsRaw(i),
            (int)sensorsPct(i),
            (int)sensorsFault(i),
            (int)c.autoOn,
            (int)c.thrPct,
            (pumpIsActive() && pumpActiveCh() == (int8_t)i) ? 1 : 0,
            schedIsQueued(i) ? 1 : 0,
            (unsigned long)pumpBudgetUsedSec(i, nowMs),
            (int)c.dryRaw,
            (int)c.wetRaw);
  }
  n = app(b, n, sizeof(b), "]}");

  b[sizeof(b) - 1] = 0;
  sendJson(b);
}

/* ================= 手动泵 ================= */
static void handlePump(void) {
  String body = s_http.arg("plain");
  long ch = jInt(body, "ch", -1);
  if (ch < 0 || ch >= CHANNEL_COUNT) { sendErr(REQ_ERR_INVALID); return; }

  char action[12];
  if (!jStr(body, "action", action, sizeof(action))) strcpy(action, "start");

  if (strcmp(action, "stop") == 0) {
    if (pumpIsActive() && pumpActiveCh() == (int8_t)ch) {
      pumpStop(STOP_MANUAL);
    } else {
      schedCancel((uint8_t)ch);   /* 还在排队的直接撤掉 */
    }
    sendJson("{\"ok\":1}");
    return;
  }

  long sec = jInt(body, "sec", storeCfg().ch[ch].maxSec);
  if (sec < 1) sec = 1;
  if (sec > HARD_MAX_SEC) sec = HARD_MAX_SEC;

  uint8_t r = schedRequest((uint8_t)ch, (uint32_t)sec * 1000UL, SRC_MANUAL);
  if (r == REQ_OK) sendJson("{\"ok\":1}");
  else sendErr(r);
}

/* ================= 通道配置 ================= */
static void handleChannel(void) {
  String body = s_http.arg("plain");
  long ch = jInt(body, "ch", -1);
  if (ch < 0 || ch >= CHANNEL_COUNT) { sendErr(REQ_ERR_INVALID); return; }

  ConfigBlob& cfg = storeCfgRW();
  ChannelCfg& c = cfg.ch[ch];

  if (body.indexOf("\"auto\"") >= 0) {
    c.autoOn = jBool(body, "auto", c.autoOn != 0) ? 1 : 0;
  }
  if (body.indexOf("\"thr\"") >= 0) {
    long v = jInt(body, "thr", c.thrPct);
    if (v < 5) v = 5;
    if (v > 90) v = 90;
    c.thrPct = (uint8_t)v;
  }
  if (body.indexOf("\"maxSec\"") >= 0) {
    long v = jInt(body, "maxSec", c.maxSec);
    if (v < 1) v = 1;
    if (v > HARD_MAX_SEC) v = HARD_MAX_SEC;
    c.maxSec = (uint16_t)v;
  }
  if (body.indexOf("\"minGapSec\"") >= 0) {
    long v = jInt(body, "minGapSec", c.minGapSec);
    if (v < 0) v = 0;
    if (v > 7200) v = 7200;
    c.minGapSec = (uint16_t)v;
  }
  if (body.indexOf("\"name\"") >= 0) {
    char nm[CH_NAME_LEN];
    if (jStr(body, "name", nm, sizeof(nm))) {
      /* 逐字符复制，只收完整的多字节字符：
         直接在字节上截断会把一个汉字切成两半，产生非法 UTF-8，
         网页显示成乱码方块，而且存进去就再也没法改回来了。 */
      char* d = c.name;
      const char* limit = c.name + sizeof(c.name) - 1;
      for (const char* s = nm; *s; ) {
        unsigned char uc = (unsigned char)*s;
        if (uc == '"' || uc == '\\') { s++; continue; }   /* 会破坏 JSON 的字符直接丢掉 */
        uint8_t len = 1;
        if (uc >= 0xF0)      len = 4;
        else if (uc >= 0xE0) len = 3;
        else if (uc >= 0xC0) len = 2;
        if (d + len > limit) break;                        /* 放不下就整个字不写 */
        for (uint8_t k = 0; k < len && s[k]; k++) *d++ = *s++;
      }
      *d = 0;
    }
  }

  storeTouch();
  sendJson("{\"ok\":1}");
}

/* ================= 校准 ================= */
static void handleCalibrate(void) {
  String body = s_http.arg("plain");
  long ch = jInt(body, "ch", -1);
  if (ch < 0 || ch >= CHANNEL_COUNT) { sendErr(REQ_ERR_INVALID); return; }

  char pt[8];
  if (!jStr(body, "point", pt, sizeof(pt))) strcpy(pt, "dry");

  /* 泵刚停的 300ms 内地弹严重，先等过去 */
  uint32_t t0 = millis();
  while (pumpEdgeRecent(millis()) && (millis() - t0) < 600) {
    delay(10);
  }

  uint8_t st = FLT_NONE;
  uint16_t med = sensorsCalibProbe((uint8_t)ch, &st);
  if (st != FLT_NONE) { sendErr(REQ_ERR_FAULT); return; }   /* 探头没接好，别把垃圾值写进去 */

  const ChannelCfg& c = storeCfg().ch[ch];
  if (strcmp(pt, "dry") == 0) {
    sensorsApplyCalib((uint8_t)ch, med, c.wetRaw);
  } else {
    sensorsApplyCalib((uint8_t)ch, c.dryRaw, med);
  }
  sensorsForceSample((uint8_t)ch);

  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":1,\"raw\":%d,\"pct\":%d}", (int)med, (int)sensorsPct((uint8_t)ch));
  sendJson(b);
}

/* ================= 一键采样标定（6 路一起） =================
 * 场景：探头全插好之后，一次把 6 路读数都记下来，
 * 不用一路一路点。泵在跑的时候拒绝，因为水泵扰动土壤 + 电源地弹都会污染读数。 */
static void handleCalibAll(void) {
  String body = s_http.arg("plain");
  char pt[8];
  if (!jStr(body, "point", pt, sizeof(pt))) strcpy(pt, "dry");
  bool isDry = (strcmp(pt, "dry") == 0);

  if (pumpIsActive()) { sendErr(REQ_ERR_BUSY); return; }

  /* 刚停泵的地弹窗口也躲开 */
  uint32_t t0 = millis();
  while (pumpEdgeRecent(millis()) && (millis() - t0) < 600) delay(10);

  uint16_t raw[CHANNEL_COUNT];
  uint8_t  st[CHANNEL_COUNT];
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    raw[i] = sensorsCalibProbe(i, &st[i]);
  }

  /* 逐路写入。某一路不合法只跳过它自己，不牵连别的通道 ——
     6 路探头新旧混用很常见，不能因为一个坏的整批标定失败。 */
  ConfigBlob& cfg = storeCfgRW();
  uint8_t applied = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (st[i] != FLT_NONE) continue;                 /* 探头没接好，跳过 */
    ChannelCfg& c = cfg.ch[i];
    if (isDry) {
      /* 干点必须比湿点高出一截，否则换算方向就反了 */
      if ((int32_t)raw[i] - (int32_t)c.wetRaw < 150) continue;
      c.dryRaw = raw[i];
    } else {
      if ((int32_t)c.dryRaw - (int32_t)raw[i] < 150) continue;
      c.wetRaw = raw[i];
    }
    logAdd(LOG_CALIB, (int8_t)i);
    applied++;
  }

  if (applied) {
    sensorsRecomputePct();
    storeFlush();                    /* 标定结果立刻落盘，不等 2 秒去抖 */
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++) sensorsForceSample(i);
  }

  char b[420];
  int n = 0;
  n = app(b, n, sizeof(b), "{\"ok\":1,\"point\":\"%s\",\"applied\":%d,\"items\":[",
          isDry ? "dry" : "wet", (int)applied);
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    n = app(b, n, sizeof(b), "%s{\"i\":%d,\"raw\":%d,\"st\":%d}",
            (i ? "," : ""), (int)i, (int)raw[i], (int)st[i]);
  }
  n = app(b, n, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  sendJson(b);
}

/* ================= 定时任务 ================= */
static void handleScheduleGet(void) {
  char b[900];
  int n = 0;
  const ConfigBlob& cfg = storeCfg();
  n = app(b, n, sizeof(b), "{\"ok\":1,\"tasks\":[");
  bool first = true;
  for (uint8_t i = 0; i < MAX_TASKS; i++) {
    const TaskCfg& t = cfg.tasks[i];
    if (!t.used) continue;
    n = app(b, n, sizeof(b),
            "%s{\"i\":%d,\"on\":%d,\"ch\":%d,\"type\":%d,\"periodMin\":%d,"
            "\"durSec\":%d,\"hour\":%d,\"min\":%d}",
            first ? "" : ",", (int)i, (int)t.on, (int)t.ch, (int)t.type,
            (int)t.periodMin, (int)t.durSec, (int)t.hour, (int)t.minute);
    first = false;
  }
  n = app(b, n, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  sendJson(b);
}

static void handleSchedulePost(void) {
  String body = s_http.arg("plain");
  ConfigBlob& cfg = storeCfgRW();

  long idx = jInt(body, "i", -1);
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
    if (!t) { sendErr(REQ_ERR_BUSY); return; }
  }

  if (body.indexOf("\"on\"") >= 0)      t->on = jBool(body, "on", t->on != 0) ? 1 : 0;
  if (body.indexOf("\"ch\"") >= 0) {
    long v = jInt(body, "ch", t->ch);
    if (v < 0) v = 0;
    if (v >= CHANNEL_COUNT) v = CHANNEL_COUNT - 1;
    t->ch = (uint8_t)v;
  }
  if (body.indexOf("interval") >= 0)    t->type = 0;
  else if (body.indexOf("daily") >= 0)  t->type = 1;

  if (body.indexOf("\"periodMin\"") >= 0) {
    long v = jInt(body, "periodMin", t->periodMin);
    if (v < 1) v = 1;
    if (v > 4320) v = 4320;
    t->periodMin = (uint16_t)v;
  }
  if (body.indexOf("\"durSec\"") >= 0) {
    long v = jInt(body, "durSec", t->durSec);
    if (v < 1) v = 1;
    if (v > HARD_MAX_SEC) v = HARD_MAX_SEC;
    t->durSec = (uint16_t)v;
  }
  if (body.indexOf("\"hour\"") >= 0) {
    long v = jInt(body, "hour", t->hour);
    if (v < 0) v = 0;
    if (v > 23) v = 23;
    t->hour = (uint8_t)v;
  }
  if (body.indexOf("\"min\"") >= 0) {
    long v = jInt(body, "min", t->minute);
    if (v < 0) v = 0;
    if (v > 59) v = 59;
    t->minute = (uint8_t)v;
  }

  t->lastRunSec = millis() / 1000UL;   /* 新任务从"现在"起算一个周期 */
  storeTouch();

  char b[48];
  snprintf(b, sizeof(b), "{\"ok\":1,\"i\":%ld}", idx);
  sendJson(b);
}

static void handleScheduleDelete(void) {
  String body = s_http.arg("plain");
  long idx = jInt(body, "i", -1);
  if (idx < 0 || idx >= MAX_TASKS) { sendErr(REQ_ERR_INVALID); return; }
  memset(&storeCfgRW().tasks[idx], 0, sizeof(TaskCfg));
  storeTouch();
  sendJson("{\"ok\":1}");
}

/* ================= 日志 ================= */
static void handleLogs(void) {
  uint32_t since = 0;
  if (s_http.hasArg("since")) since = (uint32_t)strtoul(s_http.arg("since").c_str(), nullptr, 10);

  static LogRec recs[8];
  uint16_t n = logSince(since, recs, 8);

  char b[1200];
  int w = 0;
  w = app(b, w, sizeof(b), "{\"ok\":1,\"seq\":%lu,\"items\":[",
          (unsigned long)logSeq());
  for (uint16_t i = 0; i < n; i++) {
    w = app(b, w, sizeof(b),
            "%s{\"s\":%lu,\"t\":%lu,\"c\":\"%s\",\"k\":\"%s\",\"ch\":%d}",
            (i ? "," : ""),
            (unsigned long)recs[i].s,
            (unsigned long)recs[i].t,
            logText(recs[i].code),
            logKind(recs[i].code),
            (int)recs[i].ch);
  }
  w = app(b, w, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  sendJson(b);
}

/* ================= 扫描 / 配网 / 版本 / 重启 ================= */
static void handleScan(void) {
  int n = WiFi.scanNetworks(false, false);
  char b[1100];
  int w = app(b, 0, sizeof(b), "{\"ok\":1,\"nets\":[");
  int shown = 0;
  for (int i = 0; i < n && shown < 8; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    w = app(b, w, sizeof(b), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
            (shown ? "," : ""), ssid.c_str(), WiFi.RSSI(i));
    shown++;
  }
  w = app(b, w, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;
  WiFi.scanDelete();
  sendJson(b);
}

static void handleWifi(void) {
  String body = s_http.arg("plain");
  char ssid[33];
  char pass[65];
  if (!jStr(body, "ssid", ssid, sizeof(ssid))) { sendErr(REQ_ERR_INVALID); return; }
  if (!jStr(body, "pass", pass, sizeof(pass))) pass[0] = 0;

  netSetCredentials(ssid, pass);
  sendJson("{\"ok\":1,\"saved\":1,\"rebootIn\":2}");
  webRequestReboot(2000);
}

/* ================= 热点密码 ================= */
static void handleApPassGet(void) {
  char b[128];
  snprintf(b, sizeof(b), "{\"ok\":1,\"pass\":\"%s\",\"min\":%d,\"max\":%d}",
           storeApPass(), AP_PASS_MIN_LEN, AP_PASS_MAX_LEN);
  sendJson(b);
}

static void handleApPassSet(void) {
  String body = s_http.arg("plain");
  char p[AP_PASS_MAX_LEN + 1];
  if (!jStr(body, "pass", p, sizeof(p))) { sendErr(REQ_ERR_INVALID); return; }

  size_t n = strlen(p);
  if (n < AP_PASS_MIN_LEN || n > AP_PASS_MAX_LEN) { sendErr(REQ_ERR_INVALID); return; }

  /* 只收可打印 ASCII，并排除双引号与反斜杠：前者会把外层 JSON 结构搞坏，
     后者在不同实现里的转义规则不一致。WPA2 本身也只接受 ASCII，
     中文密码在这里就被挡掉，总比设完热点直接起不来强。 */
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') {
      sendErr(REQ_ERR_INVALID);
      return;
    }
  }

  storeApPassSet(p);

  /* 先把响应发完再重开热点：重开会立刻踢掉当前这条连接，
     顺序反了前端只能看到请求失败，完全不知道密码到底改没改。 */
  sendJson("{\"ok\":1,\"saved\":1,\"reopenIn\":2}");
  s_apRestartAt = millis() + 1500;
  if (s_apRestartAt == 0) s_apRestartAt = 1;
}

/* ================= 采样周期 ================= */
static void handleCycleGet(void) {
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":1,\"min\":%u,\"lo\":%d,\"hi\":%d}",
           (unsigned)storeSampleCycle(), SAMPLE_CYCLE_MIN, SAMPLE_CYCLE_MAX);
  sendJson(b);
}

static void handleCycleSet(void) {
  String body = s_http.arg("plain");
  long v = jInt(body, "min", -1);
  if (v < SAMPLE_CYCLE_MIN || v > SAMPLE_CYCLE_MAX) { sendErr(REQ_ERR_INVALID); return; }
  storeSampleCycleSet((uint16_t)v);
  char b[48];
  snprintf(b, sizeof(b), "{\"ok\":1,\"min\":%ld}", v);
  sendJson(b);
}

static void handleVersion(void) {
  char b[160];
  snprintf(b, sizeof(b), "{\"ok\":1,\"fw\":\"%s\",\"build\":\"%s\",\"ap\":\"%s\"}",
           FW_VERSION, FW_BUILD, netApSsid());
  sendJson(b);
}

static void handleReboot(void) {
  sendJson("{\"ok\":1}");
  webRequestReboot(800);
}

static void handleNotFound(void) {
  s_http.send(404, "text/plain", "not found");
}

/* ================= 生命周期 ================= */
void webBegin(void) {
  s_http.on("/", HTTP_GET, handleRoot);
  s_http.on("/api/state", HTTP_GET, handleState);
  s_http.on("/api/pump", HTTP_POST, handlePump);
  s_http.on("/api/channel", HTTP_POST, handleChannel);
  s_http.on("/api/calibrate", HTTP_POST, handleCalibrate);
  s_http.on("/api/calib_all", HTTP_POST, handleCalibAll);
  s_http.on("/api/schedule", HTTP_GET, handleScheduleGet);
  s_http.on("/api/schedule", HTTP_POST, handleSchedulePost);
  s_http.on("/api/schedule/del", HTTP_POST, handleScheduleDelete);
  s_http.on("/api/logs", HTTP_GET, handleLogs);
  s_http.on("/api/scan", HTTP_GET, handleScan);
  s_http.on("/api/wifi", HTTP_POST, handleWifi);
  s_http.on("/api/appass", HTTP_GET, handleApPassGet);
  s_http.on("/api/appass", HTTP_POST, handleApPassSet);
  s_http.on("/api/cycle", HTTP_GET, handleCycleGet);
  s_http.on("/api/cycle", HTTP_POST, handleCycleSet);
  s_http.on("/api/version", HTTP_GET, handleVersion);
  s_http.on("/api/reboot", HTTP_POST, handleReboot);
  s_http.onNotFound(handleNotFound);

  s_http.begin();
}

void webRequestReboot(uint32_t delayMs) {
  s_rebootAt = millis() + delayMs;
  if (s_rebootAt == 0) s_rebootAt = 1;
}

void webTick(void) {
  s_http.handleClient();

  /* 热点改密后延迟重开：等上面这次响应真正发出去再动手，
     否则客户端只会收到一个断连，看不到任何结果。 */
  if (s_apRestartAt != 0 && (int32_t)(millis() - s_apRestartAt) >= 0) {
    s_apRestartAt = 0;
    netRestartAp();
    logAdd(LOG_WIFI_AP, -1);
  }

  if (s_rebootAt != 0 && (int32_t)(millis() - s_rebootAt) >= 0) {
    s_rebootAt = 0;
    storeFlush();
    delay(120);
    ESP.restart();
  }
}
