#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <string.h>

#include "config.h"
#include "types.h"
#include "net.h"
#include "config_store.h"
#include "logbuf.h"

static uint8_t  s_mode = NET_BOOT;
static uint32_t s_attemptMs = 0;
static bool     s_staTried = false;
static bool     s_apUp = false;
static bool     s_timeCfgDone = false;
static char     s_apSsid[24];

const char* netApSsid(void) { return s_apSsid; }
uint8_t netMode(void) { return s_mode; }

static void startAp(void) {
  if (s_apUp) return;
  /* WPA2 加密，密码来自 NVS（出厂 62517729，可在设置页改）。
     storeApPass() 那侧已保证返回 8~63 位，这里仍要检查返回值：
     密码非法时 softAP() 返回 false、热点根本起不来，
     设备就彻底失联了，所以退回出厂密码再试一次。 */
  if (!WiFi.softAP(s_apSsid, storeApPass())) {
    WiFi.softAP(s_apSsid, DEF_AP_PASS);
  }
  s_apUp = true;
  logAdd(LOG_WIFI_AP, -1);
}

void netBegin(void) {
  snprintf(s_apSsid, sizeof(s_apSsid), "AutoWater-%04X",
           (unsigned)((uint32_t)(ESP.getEfuseMac() & 0xFFFF)));

  WiFi.persistent(false);         /* 不要让 WiFi 库自己写 flash */
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_AP_STA);         /* AP 与 STA 共存，AP 常开 */

  startAp();

  const ConfigBlob& c = storeCfg();
  if (c.ssid[0] != 0) {
    WiFi.begin(c.ssid, c.pass);
    s_staTried = true;
    s_attemptMs = millis();
    s_mode = NET_STA_CONNECTING;
  } else {
    s_mode = NET_AP;
  }
}

void netTick(uint32_t nowMs) {
  const ConfigBlob& c = storeCfg();

  if (WiFi.status() == WL_CONNECTED) {
    if (s_mode != NET_STA) {
      s_mode = NET_STA;
      logAdd(LOG_WIFI_STA, -1);
    }
    if (!s_timeCfgDone) {
      configTime(NTP_TZ_OFFSET_SEC, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
      s_timeCfgDone = true;
    }
    return;
  }

  if (s_mode == NET_STA) {
    s_mode = s_apUp ? NET_AP : NET_STA_CONNECTING;
    logAdd(LOG_WIFI_FAIL, -1);
  }

  if (c.ssid[0] == 0) {
    s_mode = NET_AP;
    return;
  }

  /* 超时后周期性重连，不阻塞 loop */
  if (nowMs - s_attemptMs >= WIFI_RETRY_MS) {
    s_attemptMs = nowMs;
    WiFi.disconnect(false, false);
    WiFi.begin(c.ssid, c.pass);
    s_staTried = true;
    if (s_mode == NET_BOOT) s_mode = NET_STA_CONNECTING;
  }
}

void netIp(char* buf, size_t n) {
  if (!buf || n == 0) return;
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(buf, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else if (s_apUp) {
    IPAddress ip = WiFi.softAPIP();
    snprintf(buf, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else {
    snprintf(buf, n, "-");
  }
}

int netRssi(void) {
  if (WiFi.status() != WL_CONNECTED) return 0;
  return WiFi.RSSI();
}

bool netTimeSynced(void) {
  time_t t = time(nullptr);
  return (int64_t)t > 1600000000LL;
}

uint32_t netEpoch(void) {
  time_t t = time(nullptr);
  return (uint32_t)t;
}

void netSetCredentials(const char* ssid, const char* pass) {
  ConfigBlob& c = storeCfgRW();
  if (ssid) {
    strncpy(c.ssid, ssid, sizeof(c.ssid) - 1);
    c.ssid[sizeof(c.ssid) - 1] = 0;
  }
  if (pass) {
    strncpy(c.pass, pass, sizeof(c.pass) - 1);
    c.pass[sizeof(c.pass) - 1] = 0;
  }
  storeTouch();
  storeFlush();
}

/* 改完热点密码后必须重开 AP 才生效（softAP 的密码在启动时就固定了）。
 * 注意这会踢掉所有已连客户端 —— 包括正在提交改密的这台设备，
 * 所以调用方要先把 HTTP 响应发完再调这里。 */
void netRestartAp(void) {
  WiFi.softAPdisconnect(true);
  s_apUp = false;
  startAp();
}
