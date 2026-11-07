#include <Arduino.h>
#include "logbuf.h"

static LogRec   s_buf[LOG_CAP];
static uint32_t s_seq = 0;      /* 已写入的总条数，单调递增 */
static uint16_t s_head = 0;     /* 下一条写入位置 */

void logBegin(void) {
  for (uint16_t i = 0; i < LOG_CAP; i++) {
    s_buf[i].s = 0;
    s_buf[i].t = 0;
    s_buf[i].code = LOG_BOOT;
    s_buf[i].ch = -1;
    s_buf[i].d = 0;
  }
  s_seq = 0;
  s_head = 0;
}

void logAdd(uint8_t code, int8_t ch, uint16_t durSec) {
  s_seq++;
  s_buf[s_head].s = s_seq;
  s_buf[s_head].t = (uint32_t)(millis() / 1000UL);
  s_buf[s_head].code = code;
  s_buf[s_head].ch = ch;
  s_buf[s_head].d = durSec;
  s_head = (uint16_t)((s_head + 1) % LOG_CAP);
}

uint32_t logSeq(void) {
  return s_seq;
}

/* 取 seq > sinceSeq 的记录，最多 maxOut 条。返回实际条数。 */
uint16_t logSince(uint32_t sinceSeq, LogRec* out, uint16_t maxOut) {
  uint32_t total = s_seq;
  if (sinceSeq >= total) return 0;

  /* 环里现存最老的一条是 total-LOG_CAP+1 */
  uint32_t oldest = (total > LOG_CAP) ? (total - LOG_CAP + 1) : 1;
  uint32_t start = (sinceSeq + 1 > oldest) ? (sinceSeq + 1) : oldest;

  uint16_t n = 0;
  for (uint32_t s = start; s <= total && n < maxOut; s++) {
    uint16_t idx = (uint16_t)((s - 1) % LOG_CAP);
    out[n++] = s_buf[idx];
  }
  return n;
}

const char* logText(uint8_t code) {
  switch (code) {
    case LOG_BOOT:            return "设备启动";
    case LOG_RESET_REASON:    return "复位原因已记录";
    case LOG_PUMP_START:      return "开始浇水";
    case LOG_PUMP_DONE:       return "浇水完成";
    case LOG_PUMP_MANUAL_STOP:return "手动停止";
    case LOG_PUMP_PREEMPT:    return "被手动抢占";
    case LOG_PUMP_TIMEOUT:    return "撞到单次硬上限，强制停止";
    case LOG_PUMP_BUDGET:     return "超出每小时累计上限";
    case LOG_FAULT:           return "传感器故障";
    case LOG_RECOVER:         return "传感器恢复正常";
    case LOG_PWR_FAIL:        return "探头供电异常：六路均无读数，检查供电继电器";
    case LOG_PWR_OK:          return "探头供电已恢复";
    case LOG_CALIB:           return "校准点已记录";
    case LOG_CFG_SAVE:        return "配置已保存";
    case LOG_WIFI_STA:        return "已连上路由器";
    case LOG_WIFI_AP:         return "热点已启动";
    case LOG_WIFI_FAIL:       return "路由器连接失败，改用热点";
    case LOG_TASK_FIRE:       return "定时任务触发";
    case LOG_TASK_SKIP:       return "定时任务错过窗口，已丢弃";
    case LOG_LOCKOUT:         return "检测到带泵复位，进入锁定";
    case LOG_CFG_BAD:         return "配置校验失败，已回退默认值";
    case LOG_MQTT:            return "MQTT 连接状态变化";
    default:                  return "未知事件";
  }
}

const char* logKind(uint8_t code) {
  switch (code) {
    case LOG_PUMP_DONE:
    case LOG_RECOVER:
    case LOG_CALIB:
    case LOG_CFG_SAVE:
    case LOG_WIFI_STA:
    case LOG_TASK_FIRE:
      return "ok";
    case LOG_PUMP_PREEMPT:
    case LOG_PUMP_TIMEOUT:
    case LOG_FAULT:
    case LOG_PWR_OK:
    case LOG_WIFI_AP:
    case LOG_TASK_SKIP:
    case LOG_CFG_BAD:
      return "warn";
    case LOG_PUMP_BUDGET:
    case LOG_PWR_FAIL:
    case LOG_WIFI_FAIL:
    case LOG_LOCKOUT:
    case LOG_MQTT:
      return "err";
    default:
      return "";
  }
}
