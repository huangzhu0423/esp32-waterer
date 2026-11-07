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
static char      s_in[256];             /* 命令回调的临时缓冲 */

/* ================= 极简 JSON 拼接（与 web_server 的 app 同款） ================= */
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

/* ================= 命令回调 ================= */
static void onMessage(char* topic, byte* payload, unsigned int len) {
  if (len == 0 || len >= sizeof(s_in)) return;
  memcpy(s_in, payload, len);
  s_in[len] = 0;
  String body = String((const char*)s_in);

  /* 令牌校验：配置了 token 就必须带对，否则整条丢弃。
     这一步是「暴露到公网/公共 broker」前的必需防线——
     否则任何能连上同一 broker 的人都能开你的泵、抽干水箱。 */
  char tk[MQTT_TOKEN_MAX_LEN + 1];
  bool hasTok = jUtilStr(body, "t", tk, sizeof(tk));
  if (s_token[0] != 0) {
    if (!hasTok || strncmp(tk, s_token, MQTT_TOKEN_MAX_LEN) != 0) {
      return;   /* 令牌不对，静默忽略 */
    }
  }

  char act[12];
  if (!jUtilStr(body, "a", act, sizeof(act))) strcpy(act, "start");

  long ch = jUtilInt(body, "ch", -1);
  if (ch < 0 || ch >= CHANNEL_COUNT) return;

  if (strcmp(act, "stop") == 0) {
    if (pumpIsActive() && pumpActiveCh() == (int8_t)ch) {
      pumpStop(STOP_MANUAL);
    } else {
      schedCancel((uint8_t)ch);   /* 还在排队的撤掉 */
    }
    s_publishNow = true;
    return;
  }

  if (strcmp(act, "auto") == 0) {
    bool on = jUtilBool(body, "on", true);
    ConfigBlob& cfg = storeCfgRW();
    cfg.ch[ch].autoOn = on ? 1 : 0;
    storeTouch();
    s_publishNow = true;
    return;
  }

  /* 默认 action = start */
  long sec = jUtilInt(body, "s", storeCfg().ch[ch].maxSec);
  if (sec < 1) sec = 1;
  if (sec > HARD_MAX_SEC) sec = HARD_MAX_SEC;

  uint8_t r = schedRequest((uint8_t)ch, (uint32_t)sec * 1000UL, SRC_MANUAL);
  if (r == REQ_OK) s_publishNow = true;
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
           "{\"fw\":\"%s\",\"up\":%lu,\"heap\":%lu,\"mode\":\"%s\",\"ip\":\"%s\","
           "\"rssi\":%d,\"samp\":%d,\"next\":%lu,",
           FW_VERSION,
           (unsigned long)(nowMs / 1000UL),
           (unsigned long)ESP.getFreeHeap(),
           (netMode() == NET_STA) ? "sta" : "ap",
           ip,
           netRssi(),
           sensorsSampling() ? 1 : 0,
           (unsigned long)(sensorsNextCycleMs(nowMs) / 1000UL));

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
             "%s{\"i\":%d,\"raw\":%d,\"pct\":%d,\"flt\":%d,\"auto\":%d,\"run\":%d}",
             (i ? "," : ""),
             (int)i,
             (int)sensorsRaw(i),
             (int)sensorsPct(i),
             (int)sensorsFault(i),
             (int)c.autoOn,
             (pumpIsActive() && pumpActiveCh() == (int8_t)i) ? 1 : 0);
  }
  n = app2(b, n, sizeof(b), "]}");
  b[sizeof(b) - 1] = 0;

  char top[MQTT_BASE_MAX_LEN + 16];
  topicName(top, sizeof(top), "state");
  s_mqtt.publish(top, b, false);
}

/* ================= 连接尝试 ================= */
static void attemptConnect(uint32_t nowMs) {
  s_wc.setTimeout(3000);          /* 兜底：不可达的 broker 最多卡 3 秒 */
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
  /* 订阅命令主题，并广播一次在线 + 当前状态 */
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
  /* 默认主题前缀：aw_ + 芯片 ID 后四位（与热点名 AutoWater-XXXX 呼应） */
  uint32_t mac = (uint32_t)(ESP.getEfuseMac() & 0xFFFF);
  snprintf(s_defBase, sizeof(s_defBase), "aw_%04X", (unsigned)mac);

  /* 先给兜底值：NVS 打不开也不能退化成空配置导致连不上 */
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
}

void mqttTick(uint32_t nowMs) {
  if (!s_en || s_host[0] == 0) {
    /* 没启用或没配服务器：确保断开，什么都不做 */
    if (s_conn) { s_mqtt.disconnect(); s_conn = false; }
    return;
  }

  /* 必须连上路由器才能用 MQTT；退化为热点时直接断开，不空耗 */
  if (netMode() != NET_STA) {
    if (s_conn) { s_mqtt.disconnect(); s_conn = false; }
    return;
  }

  if (!s_conn) {
    if ((int32_t)(nowMs - s_nextConnMs) < 0) return;   /* 退避中 */
    attemptConnect(nowMs);
    return;
  }

  /* 已连接：喂狗 + 周期上报 + 变更上报 */
  s_mqtt.loop();

  /* 变更检测：泵启停、故障掩码变化，都立刻上报一次 */
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

  if (s_publishNow) {
    s_publishNow = false;
    publishState();
  }
}

void mqttPublishState(void) {
  s_publishNow = true;
}

bool mqttEnabled(void)  { return s_en; }
bool mqttConnected(void) { return s_conn; }

/* ================= 配置读写 ================= */
bool mqttCfgEnabled(void) { return s_en; }
void mqttCfgEnabledSet(bool on) {
  s_en = on;
  if (s_pref.begin("mqtt", false)) { s_pref.putBool("en", on); s_pref.end(); }
  /* 关闭后下一拍 mqttTick 会自动断开 */
}

const char* mqttCfgHost(void) { return s_host; }
void mqttCfgHostSet(const char* h) {
  if (!h) return;
  strncpy(s_host, h, MQTT_HOST_MAX_LEN);
  s_host[MQTT_HOST_MAX_LEN] = 0;
  if (s_pref.begin("mqtt", false)) { s_pref.putString("host", s_host); s_pref.end(); }
  s_nextConnMs = 0;   /* 改了地址立刻重连 */
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
  if (!b) { return; }
  char tmp[MQTT_BASE_MAX_LEN + 1];
  strncpy(tmp, b, MQTT_BASE_MAX_LEN);
  tmp[MQTT_BASE_MAX_LEN] = 0;
  /* 空字符串表示回落默认前缀 */
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
