#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "types.h"
#include "health.h"
#include "config_store.h"
#include "logbuf.h"

static uint8_t  s_resetReason = 0;
static uint32_t s_lockUntilMs = 0;
static bool     s_lockAnnounced = false;
static bool     s_wdtOn = false;

uint8_t healthResetReason(void) { return s_resetReason; }

const char* healthResetText(void) {
  switch (s_resetReason) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "程序异常";
    case ESP_RST_INT_WDT:   return "中断看门狗";
    case ESP_RST_TASK_WDT:  return "任务看门狗";
    case ESP_RST_WDT:       return "看门狗";
    case ESP_RST_BROWNOUT:  return "电压跌落";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_EXT:       return "外部复位";
    default:                return "未知";
  }
}

void healthBegin(void) {
  s_resetReason = (uint8_t)esp_reset_reason();
  logAdd(LOG_RESET_REASON, -1);

  /* 上一次复位时泵还在转 → 说明是带泵掉电/看门狗复位，不是正常关机。
     这种情况下不能马上继续浇水，先冷静一段时间。 */
  if (storeRunMarkerGet()) {
    uint8_t bad = storeBadBootCount();
    if (bad < 250) bad++;
    storeBadBootCountSet(bad);

    s_lockUntilMs = (bad >= 2) ? LOCKOUT_SECOND_MS : LOCKOUT_FIRST_MS;
    logAdd(LOG_LOCKOUT, -1);
  } else {
    storeBadBootCountSet(0);
    s_lockUntilMs = 0;
  }
  storeRunMarkerSet(false);   /* 清标记，避免下一次误判 */
  s_lockAnnounced = false;
}

void healthTick(uint32_t nowMs) {
  if (s_lockUntilMs > 0 && nowMs >= s_lockUntilMs) {
    s_lockUntilMs = 0;
    storeBadBootCountSet(0);
    if (!s_lockAnnounced) {
      s_lockAnnounced = true;
      logAdd(LOG_RECOVER, -1);
    }
  }
}

bool healthLocked(uint32_t nowMs) {
  return (s_lockUntilMs > 0) && (nowMs < s_lockUntilMs);
}

uint32_t healthLockRemainSec(uint32_t nowMs) {
  if (!healthLocked(nowMs)) return 0;
  return (s_lockUntilMs - nowMs) / 1000UL;
}

/* ---- 任务看门狗 ----
 * core 2.x 与 core 3.x 的 esp_task_wdt_init 签名不同，这里按版本分支。 */
void healthWatchdogBegin(void) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  esp_task_wdt_config_t cfg;
  cfg.timeout_ms = (uint32_t)WDT_TIMEOUT_SEC * 1000UL;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;

  /* core 3.x 启动时已经初始化过 TWDT（默认 5 秒、loopTask 已挂上）。
     此时再调 esp_task_wdt_init 只会返回 INVALID_STATE 并在串口打一条 E 级日志，
     超时值仍然是 5 秒 —— 所以先 reconfigure 把超时改成我们要的 10 秒。 */
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);
  if (e != ESP_OK) e = esp_task_wdt_init(&cfg);
  if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;
#else
  esp_err_t e = esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;   /* 已被初始化过，忽略 */
#endif

  if (e == ESP_OK) {
    /* loopTask 可能已经在 TWDT 上了（core 3.x 默认就会挂），
       这种"重复订阅"会返回 INVALID_ARG，但对我们要做的事没有影响：
       只要能在 loop 里喂狗就行，所以两种返回都算成功。 */
    esp_err_t a = esp_task_wdt_add(NULL);
    s_wdtOn = (a == ESP_OK) || (a == ESP_ERR_INVALID_ARG);
  }
}

void healthWatchdogFeed(void) {
  if (s_wdtOn) esp_task_wdt_reset();
}
