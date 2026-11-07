/*
 * ESP32 六路自动浇水系统 —— 主程序
 *
 * 这里只做初始化和主循环调度，不放任何业务逻辑。
 * 各模块职责：
 *   config.h        引脚表与安全常数
 *   sensors.*       湿度采样、校准、故障判定
 *   pump.*          水泵驱动与全局互斥令牌
 *   scheduler.*     手动 / 定时 / 自动 三源仲裁
 *   net.*           WiFi 状态机（AP 常开 + STA 后台连接）
 *   web_server.*    HTTP 与 REST 接口
 *   config_store.*  NVS 持久化
 *   health.*        看门狗与异常掉电锁定
 */
#include <Arduino.h>

#include "config.h"
#include "types.h"
#include "logbuf.h"
#include "config_store.h"
#include "health.h"
#include "sensors.h"
#include "pump.h"
#include "scheduler.h"
#include "net.h"
#include "web_server.h"

void setup(void) {
  /* 第一件事：把所有水泵脚拉到低电平。
     从复位到这一行之间有 200~300ms 的 bootloader 窗口，软件管不到，
     那段时间靠每路 MOS 栅极的 10k 下拉电阻保证泵不吸合。 */
  pumpBegin();

  Serial.begin(115200);
  delay(30);

  logBegin();

  /* 引脚合法性检查：编译期做不了（要兼容 C++11），所以在这里跑一次 */
  const char* pinErr = cfgValidatePins();
  if (pinErr != nullptr) {
    Serial.print("[FATAL] 引脚配置错误: ");
    Serial.println(pinErr);
    /* 引脚错了就绝不能让泵乱开，直接停在这里，等人工修 config.h */
    for (;;) {
      delay(1000);
    }
  }

  storeBegin();      /* 先读配置，后面很多模块都依赖它 */
  sensorsBegin();
  healthBegin();     /* 依赖 store 判断"带泵复位" */
  schedBegin();
  netBegin();
  webBegin();
  healthWatchdogBegin();

  logAdd(LOG_BOOT, -1);

  char ip[20];
  netIp(ip, sizeof(ip));

  Serial.println();
  Serial.println("=====================================");
  Serial.print("自动浇水固件 v");
  Serial.println(FW_VERSION);
  Serial.print("复位原因: ");
  Serial.println(healthResetText());
  Serial.print("热点名称: ");
  Serial.println(netApSsid());
  Serial.print("当前地址: ");
  Serial.println(ip);
  Serial.println("=====================================");
}

void loop(void) {
  uint32_t now = millis();

  /* 顺序有讲究：
     1 先处理系统健康（锁定到期、复位原因）
     2 网络状态机
     3 传感器采样（要避开泵启停的地弹窗口）
     4 仲裁与开泵
     5 网页请求
     6 配置落盘 */
  healthTick(now);
  netTick(now);
  sensorsTick(now, pumpEdgeRecent(now));
  schedTick(now);
  webTick();
  storeTick(now);

  healthWatchdogFeed();
  delay(2);
}
