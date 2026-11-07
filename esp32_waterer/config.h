#pragma once
/*
 * config.h —— 全部编译期常量与引脚表
 * 这里不放任何业务逻辑，只放"物理事实"和"安全边界"。
 */
#include <stdint.h>

/* ================= 固件信息 ================= */
#define FW_VERSION   "1.1.2"
#define FW_BUILD     __DATE__ " " __TIME__

/* ================= 规模 ================= */
#define CHANNEL_COUNT   6     /* 传感器 + 水泵，一一对应 */
#define MAX_TASKS       8     /* 定时任务槽位 */

/* ================= 传感器引脚（必须全部在 ADC1） =================
 * ESP32 的 ADC2 在 WiFi 开启时完全不可用，所以只能用 ADC1。
 * ADC1 = GPIO32/33/34/35/36/37/38/39，其中 37/38 在 30 针板上未引出，
 * 因此实际可用 6 个 —— 这就是 CHANNEL_COUNT 的硬件上限。
 * 注意：36/39/34/35 是 input-only，没有内部上下拉，
 *       探头拔掉时引脚会浮空乱飘，所以每路外部必须加 100k 泄放电阻到 GND。
 *
 * 实测板型：ESP32-WROOM-32 DevKit 30 针（USB-C，丝印为 D 前缀写法），
 * 括号里是这块板上的丝印名，照丝印接线即可。
 */
#define PIN_ADC_0   36    /* VP   input-only */
#define PIN_ADC_1   39    /* VN   input-only */
#define PIN_ADC_2   34    /* D34  input-only */
#define PIN_ADC_3   35    /* D35  input-only */
#define PIN_ADC_4   32    /* D32 */
#define PIN_ADC_5   33    /* D33 */

/* ================= 水泵引脚（低边 MOS 开关，高电平开泵） =================
 * 全部避开：6~11(SPI Flash)、0/2/12/15(strapping)、4/5(上电PWM)、
 *          1/3(UART)、14(上电PWM)、16/17(WROVER 的 PSRAM)。
 */
#define PIN_PUMP_0  13    /* D13 */
#define PIN_PUMP_1  18    /* D18 */
#define PIN_PUMP_2  19    /* D19 */
#define PIN_PUMP_3  23    /* D23 */
#define PIN_PUMP_4  25    /* D25 */
#define PIN_PUMP_5  26    /* D26 */

static const uint8_t PIN_ADC[CHANNEL_COUNT]  = {
  PIN_ADC_0, PIN_ADC_1, PIN_ADC_2, PIN_ADC_3, PIN_ADC_4, PIN_ADC_5
};
static const uint8_t PIN_PUMP[CHANNEL_COUNT] = {
  PIN_PUMP_0, PIN_PUMP_1, PIN_PUMP_2, PIN_PUMP_3, PIN_PUMP_4, PIN_PUMP_5
};

/* ================= 水泵驱动电平 =================
 * 0 = 高电平开泵 —— 低边 N-MOS 栅极，也是继电器"高电平触发"跳线（默认）
 * 1 = 低电平开泵 —— 继电器模块的"低电平触发"（光耦把 IN 拉低才吸合）
 *
 * ！！接低电平触发的继电器模块时必须改成 1 ！！
 * 否则固件把引脚拉低当作"关"，模块却认为"开"，
 * 上电之后泵会一直转，直到你把电拔了。
 */
#define PUMP_ACTIVE_LOW 0

#if PUMP_ACTIVE_LOW
  #define PUMP_ON_LEVEL  LOW
  #define PUMP_OFF_LEVEL HIGH
#else
  #define PUMP_ON_LEVEL  HIGH
  #define PUMP_OFF_LEVEL LOW
#endif

/* ================= 输出扩展（30 路时启用） =================
 * 0 = 直接驱动 GPIO（当前 6 路用这个，安全可用的输出脚约 13 个）
 * 1 = 经 MCP23017（I2C）或 74HC595（移位）扩展
 * 打开后只需改 pump.cpp 里 pumpHwApply() 的实现，
 * 调度、互斥、预算这些业务逻辑一行都不用动。 */
#define USE_IO_EXPANDER 0

/* ================= MUX 预留（本次不接） =================
 * 将来要突破 6 路时：把 USE_MUX 改 1、接 CD74HC4067、
 * 只改 sensorsReadRaw() 一个函数，其余业务代码零改动。
 * 注意 PIN_MUX_S0 与 PIN_SENSOR_PWR 都占了 GPIO27，
 * 启用 MUX 时要把其中一个挪到别的脚。
 */
#define USE_MUX 0
#if USE_MUX
  #define PIN_MUX_S0   27
  #define PIN_MUX_S1   21
  #define PIN_MUX_S2   22
  #define PIN_MUX_S3    2
  #define PIN_MUX_SIG  PIN_ADC_0
#endif

/* ================= 安全硬上限（不可被网页配置突破） ================= */
#define HARD_MAX_SEC              300   /* 单次最长浇水 5 分钟 */
#define INTER_PUMP_GAP_MS        2000   /* 两次开泵之间至少隔 2 秒，等电源恢复 */
#define ADC_SETTLE_MS             300   /* 泵启停后 300ms 内不采 ADC（地弹窗口） */
#define PREEMPT_MIN_MS           3000   /* 已被占用的泵至少跑 3 秒才允许被抢占 */
#define MAX_WAIT_MS            120000   /* 排队超 120 秒的定时任务直接丢弃 */
#define CH_BUDGET_SEC_PER_HOUR    900   /* 每通道每小时累计上限 15 分钟 */
#define GLOBAL_BUDGET_SEC_PER_HOUR 1800 /* 全局每小时累计上限 30 分钟 */

/* ================= 传感器采样 ================= */
#define ADC_SAMPLE_INTERVAL_MS    500   /* 每路 500ms 采一次，6 路轮流 */

/* ================= 探头间歇供电 =================
 * 电容式探头长期通电会加速极板电化学腐蚀，6 路常供合计约 30mA。
 * 改成一路上电、采完立刻断电。
 *
 * ！硬件必须配合，否则下面这些配置不起作用 ！
 *
 * 方案 A：继电器模块（接线最简单，推荐）
 *   3V3  ── 继电器 COM
 *   继电器 NO ── 传感器 VCC 母线（六路探头 VCC 并在一起）
 *   继电器 IN ── GPIO27       模块 VCC 接 3V3（线圈 3.3V 版）、GND 共地
 *   断开 VCC 是彻底隔离，AOUT 不会经内部电路倒灌，比断 GND 安全得多。
 *   常见模块是"高电平触发"，即 SENSOR_PWR_ACTIVE_LOW = 0；
 *   模块上有 H/L 跳线的，按跳线实际位置设。
 *   注意先看清继电器外壳印的线圈电压：3.3VDC 版接 3V3，5VDC 版接 5V，
 *   接错会烧线圈。3.3V 线圈单只吸合约 73mA，吸合瞬间建议在模块 VCC 旁
 *   并 100~470uF 缓冲，否则浪涌可能把 3V3 拉低导致 ESP32 复位。
 *
 * 方案 B：P-MOS 高边开关（体积小、无触点磨损，但要会焊）
 *   3V3 ──[S] P-MOS [D]── 传感器 VCC 母线
 *              [G] ── GPIO27，且 G 经 10k 上拉到 3V3
 *   GPIO 拉低才导通，此时设 SENSOR_PWR_ACTIVE_LOW = 1。
 *
 * 两种方案都不能用 N-MOS 断 GND：探头断电后 AOUT 仍会经内部电路带电压，
 * 电流会从 ADC 引脚的 ESD 保护二极管倒灌进芯片，读数和寿命都完蛋。
 *
 * 继电器寿命提醒：机械触点约 10 万次，30 分钟一轮 ≈ 1.75 万次/年，
 * 加上浇水后补采，大约 4~5 年到期。另外 30mA 属"干电流"，银触点不会
 * 起弧自清洁，长期可能氧化导致接触电阻变大——若发现六路读数同时偏高，
 * 先量一下触点两端压降。想彻底免除这项维护就换 P-MOS。
 *
 * 注意 GPIO27 与 USE_MUX 的 PIN_MUX_S0 撞车，将来启用多路复用要换脚。 */
#define PIN_SENSOR_PWR        27

/* 0 = 高电平吸合 —— 继电器模块"高电平触发"跳线（多数模块出厂默认）
 * 1 = 低电平有效 —— 继电器"低电平触发"，或 P-MOS 高边开关栅极
 * 复位期间 GPIO27 处于高阻：高触发模块的 IN 自带下拉、低触发模块的 IN
 * 自带上拉，两种情况继电器都保持释放，探头不会意外上电。 */
#define SENSOR_PWR_ACTIVE_LOW   0

#if SENSOR_PWR_ACTIVE_LOW
  #define SENSOR_PWR_ON_LEVEL  LOW
  #define SENSOR_PWR_OFF_LEVEL HIGH
#else
  #define SENSOR_PWR_ON_LEVEL  HIGH
  #define SENSOR_PWR_OFF_LEVEL LOW
#endif

/* 采样周期：运行时可在网页调，这里只定义默认值与边界。
 * 单独存 NVS 的 "cycle" 键，不进 ConfigBlob——否则改一次参数就要重置整份配置。
 * 下限 5 分钟：再短就是白耗继电器触点寿命，土湿变化也没那么快。
 * 上限 6 小时：再长会拿着很久以前的读数判断，容易把植物渴过头。 */
#define DEF_SAMPLE_CYCLE_MIN   30    /* 出厂 30 分钟一轮 */
#define SAMPLE_CYCLE_MIN        5
#define SAMPLE_CYCLE_MAX      360
#define SENSOR_SETTLE_MS      500    /* 上电后等探头输出稳定（内部有滤波电容） */
#define SENSOR_READ_GAP_MS     20    /* 相邻两次采样的间隔，给 ADC 采样电容充电 */
#define SENSOR_REPEAT           3    /* 每路连采几遍：让故障计数能在同一轮里攒满 */

#define ADC_OVERSAMPLE             9    /* 9 次取中位数 */
/* 低于此值判为断线/短路到地。
 * 原来是 100，实测六路都没接探头时悬空 ADC 会漂到 100~107，
 * 刚好越过阈值被当成"正常读数"，再由 (dry-raw)/span 算出 100% 湿度，
 * 看起来像两路浇透了、还能点浇水。HW-390 泡在水里最低也约 1000~1300，
 * 取 400 既能挡住悬空噪声，又离最湿的真实读数留足余量。 */
#define ADC_OPEN_RAW             400
#define ADC_SHORT_RAW           4000    /* 高于此值判为短路到 3V3 */
#define ADC_JUMP_RAW            1500    /* 单次跳变超过此值视为噪声，丢弃 */
/* 断线/短路是明确的电气状态，一次异常即判故障。
 * 原来是 3：一轮每路只采 SENSOR_REPEAT=3 遍，采样中途被打断时计数攒不满，
 * 最后那一路就会以 flt=0 出现在网页和 MQTT 状态帧上，
 * 看起来像"接线正常"、还能点浇水。解除故障仍需 FAULT_RECOVER_COUNT
 * 次连续正常样本，所以不会因偶发抖动来回跳。 */
#define FAULT_CONFIRM_COUNT        1
#define FAULT_RECOVER_COUNT        3    /* 连续 3 次正常才解除故障 */
#define STUCK_SAMPLES             60    /* 连续 60 次（约 30 秒）读数一字不差判为卡死。
                                           土湿几十秒内本就几乎不变，不能用"变化量小"判卡死，
                                           真实 ADC 至少有 ±1 LSB 抖动才是可靠判据。 */
#define NOISY_LIMIT_PER_MIN       12    /* 一分钟丢弃样本超过 12 次判为噪声故障 */

/* ================= 默认配置（首次上电 / 配置损坏时使用） ================= */
/* 2 = 通道结构加入 type 字段（湿度/pH）
 * 3 = 通道结构加入 name 备注字段
 * 结构体大小变了，老设备读到旧 blob 会因长度不符自动回落默认值，
 * 属于预期行为——升级后需要重新配网、重新校准。 */
#define CFG_VERSION                3
#define DEF_THR_PCT               30
#define DEF_MAX_SEC               15
#define DEF_MIN_GAP_SEC          300

/* 传感器型号：Capacitive Soil Moisture V2.0.0（HW-390），3V3 供电。
 * 这个型号的输出比常见 v1.2 更线性，在 ESP32 12 位 ADC 上的典型区间：
 *   干空气 约 2600~2900，泡在水里 约 1000~1300。
 * 下面两个值取区间中值，只作为出厂兜底；每片探头差异不小，
 * 装好后照 README 的校准流程各按一次按钮才是准的。 */
#define DEF_DRY_RAW             2800    /* 干土读数（大） */
#define DEF_WET_RAW             1150    /* 湿土读数（小） */
#define DEF_AUTO_ON                1

/* 热点密码。出厂默认 62517729，装好后可在网页设置页改。
 * WPA2 硬性要求 8~63 个 ASCII 字符——少于 8 位时 softAP() 会直接失败，
 * 热点根本起不来，所以下面两个边界不能改小。
 * 注意这个值单独存 NVS（key = "appass"），不进 ConfigBlob：
 * 往结构体里加字段会改变 sizeof，老设备的整份配置会因校验失败被重置。 */
#define DEF_AP_PASS             "62517729"
#define AP_PASS_MIN_LEN            8
#define AP_PASS_MAX_LEN           63

/* ================= 其它 ================= */
#define WIFI_STA_TIMEOUT_MS     20000   /* STA 连接超时后开始周期性重试 */
#define WIFI_RETRY_MS           60000

/* ================= MQTT 远程控制 =================
 * 走公共 broker（巴法云 / EMQ 公共服 / 自建），板子连上路由器后主动连，
 * 手机在外网就能看湿度、下发浇水指令，不需要任何常开电脑或公网 IP。
 * 配置单独存 NVS 命名空间 "mqtt"，不进 ConfigBlob，改这个不会重置主配置。 */
#define MQTT_BUF_SIZE       2048   /* 收发缓冲：状态帧约 1KB，日志帧 12 条带绝对时间/时长最长约 1.7KB */
#define MQTT_PUB_MS        30000   /* 状态上报节流：最多每 30 秒一次 */
#define MQTT_RETRY_MS       5000   /* 连接失败后 5 秒重试 */
#define MQTT_PORT_DFLT     1883
/* 主题前缀默认 aw_XXXX（XXXX=芯片 ID 后四位），与热点名呼应；
 * 同一 broker 上别人猜到前缀也改不了什么，但指令必须带正确令牌。 */
#define MQTT_HOST_MAX_LEN     64
#define MQTT_USER_MAX_LEN     40
#define MQTT_PASS_MAX_LEN     40
#define MQTT_TOKEN_MAX_LEN    40
#define MQTT_BASE_MAX_LEN     40
#define CFG_SAVE_DEBOUNCE_MS     2000   /* 配置改动后 2 秒才落盘，避免连续滑动条刷写 */
#define WDT_TIMEOUT_SEC            10
#define LOCKOUT_FIRST_MS       300000   /* 带泵复位后锁定 5 分钟 */
#define LOCKOUT_SECOND_MS     1800000   /* 连续两次带泵复位锁定 30 分钟 */
#define NTP_TZ_OFFSET_SEC       28800   /* 东八区 */

/* ===================== 引脚合法性校验 ===================== */
inline bool cfgIsAdc1(uint8_t p) {
  return (p >= 32 && p <= 39);
}

inline bool cfgIsInputOnly(uint8_t p) {
  return (p == 34 || p == 35 || p == 36 || p == 37 || p == 38 || p == 39);
}

/* 这些引脚不能用作输出 */
inline bool cfgIsForbiddenOutput(uint8_t p) {
  if (p >= 6 && p <= 11) return true;               /* SPI Flash */
  if (p == 0 || p == 2 || p == 12 || p == 15) return true;  /* strapping */
  if (p == 1 || p == 3) return true;                /* UART0 */
  if (p == 4 || p == 5) return true;                /* strapping 组，上电有 PWM */
  if (p == 14) return true;                         /* 上电 PWM */
  if (p == 16 || p == 17) return true;              /* WROVER PSRAM */
  if (p == 20 || p == 24) return true;              /* 部分模组未引出 */
  if (cfgIsInputOnly(p)) return true;
  return false;
}

/* 返回 nullptr 表示通过，否则返回错误说明。
 * 编译期没法做（要兼容 C++11），所以放在 setup() 里跑一次。 */
inline const char* cfgValidatePins() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (!cfgIsAdc1(PIN_ADC[i])) return "传感器引脚不在 ADC1，WiFi 开启时 ADC2 不可用";
    for (uint8_t j = 0; j < CHANNEL_COUNT; j++) {
      if (i != j && PIN_ADC[i] == PIN_ADC[j]) return "传感器引脚重复";
    }
    if (cfgIsForbiddenOutput(PIN_PUMP[i])) return "水泵引脚是禁用输出引脚";
    for (uint8_t j = 0; j < CHANNEL_COUNT; j++) {
      if (i != j && PIN_PUMP[i] == PIN_PUMP[j]) return "水泵引脚重复";
      if (PIN_PUMP[i] == PIN_ADC[j]) return "水泵引脚与传感器引脚冲突";
      if (PIN_PUMP[i] == PIN_SENSOR_PWR) return "水泵引脚与传感器供电脚冲突";
    }
  }
  if (cfgIsForbiddenOutput(PIN_SENSOR_PWR)) return "传感器供电引脚是禁用输出引脚";
  for (uint8_t j = 0; j < CHANNEL_COUNT; j++) {
    if (PIN_SENSOR_PWR == PIN_ADC[j]) return "传感器供电脚与传感器引脚冲突";
  }
  return nullptr;
}

static_assert(CHANNEL_COUNT <= 6, "纯 GPIO 方案传感器最多 6 路，超过需启用 USE_MUX");
static_assert(MAX_TASKS <= 16, "任务槽位过多会挤占 NVS blob 空间");
