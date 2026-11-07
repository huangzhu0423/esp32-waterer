# ESP32 六路自动浇水系统

阳台 / 室内盆栽用的自动浇水控制器。ESP32 自己托管手机网页，手机连上就能看湿度、手动浇、设阈值、排定时，**不依赖任何外部服务器或公网**。

- 6 路完全独立：每路 1 个电容式土壤湿度传感器 + 1 个 5V 小泵
- 三种浇水方式共存：湿度阈值自动、定时任务、网页手动
- 全局互斥：任何时刻最多一路泵运行，靠 5V/2A 单电源就够
- 双模联网：AP 热点常开 + 后台连家里路由器

## 目录

```
esp32_waterer/          ← Arduino IDE 直接打开这个文件夹
  esp32_waterer.ino     主程序，只做初始化与调度
  config.h              引脚表 + 全部安全常数（要改引脚只改这里）
  types.h               枚举与结构体
  sensors.*             采样、校准、故障判定
  pump.*                水泵驱动与互斥令牌
  scheduler.*           手动/定时/自动 三源仲裁
  config_store.*        NVS 持久化 + CRC
  net.*                 WiFi 状态机
  web_server.*          HTTP + REST
  mqtt_link.*           MQTT 远程控制链路（PubSubClient）
  jsonutil.h            极简 JSON 取值（web_server 与 mqtt_link 共用）
  logbuf.*              RAM 环形日志
  health.*              看门狗 + 掉电锁定
  web_ui.h              【自动生成】内嵌网页，勿手改

web/index.html          手机控制页源文件（双击可直接预览）
tools/build_web.py      index.html → gzip → web_ui.h
tools/check_firmware.py 静态检查门禁
docs/WIRING.md          接线表（先看这个）
docs/API.md             接口文档
```

## 烧录

### 方式一：Arduino IDE

1. 装 Arduino IDE 2.x，在「首选项 → 附加开发板管理器网址」加入
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. 开发板管理器搜索 `esp32`，安装 **esp32 by Espressif Systems**（2.0.x 或 3.x 都行）
3. 打开 `esp32_waterer/esp32_waterer.ino`，开发板选 `ESP32 Dev Module`，上传

### 方式二：命令行

项目里已经放了一份免安装的 `arduino-cli.exe`（`.tools/arduino-cli.exe`），装核心包后即可编译：

```bash
.tools/arduino-cli.exe config init
.tools/arduino-cli.exe core update-index \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
.tools/arduino-cli.exe core install esp32:esp32 \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json

.tools/arduino-cli.exe compile --fqbn esp32:esp32:esp32 esp32_waterer
.tools/arduino-cli.exe upload  --fqbn esp32:esp32:esp32 -p COM3 esp32_waterer
```

> 核心包 48MB + 编译器 320MB，都在 GitHub 上。国内网速慢的话这一步要等一阵，可以把 `esp32:esp32` 换成 `esp32:esp32@2.0.17` 试试旧版本（体积小一些）。

### 方式三：PlatformIO

```bash
pio run -t upload
```

## 首次上电

1. 手机搜 WiFi，连 **`AutoWater-XXXX`**（无密码，XXXX 是芯片 ID 后四位）
2. 浏览器打开 `http://192.168.4.1`
3. 进「设置」页填家里 2.4G WiFi 的账号密码，保存后板子自动重启
4. 重启后手机连回家里 WiFi，用设置页显示的 IP 访问（路由器里也能看到设备名 `AutoWater-XXXX`）

> 热点是**常开**的，不会因为连上路由器就关掉。这样即使换了路由器密码、或者路由器坏了，你永远能连热点进去改配置，不用拆下来重新烧录。

## 校准（很重要，一分钟搞定）

电容式土壤传感器的干湿读数各家都不一样，所以固件不硬编码，靠现场标定。

**一键标定（推荐，6 路一起）**

1. 6 个探头都插进花盆的土里（自然偏干状态），按通道页顶部的「记录为干土」
2. 6 路读数同时记下来，环形百分比立刻按新干点重算
3. 手动各浇一次、等土吸透，再按「记录为已浇透」

有探头没接好（读数 < 400 或 > 4000）会被自动跳过，页面提示跳了几路，不影响其它通道；正在浇水时按下会提示等浇完再标。标定结果立即写入 flash，断电不丢。

**单路标定**：点某一路卡片右上角「⋯」，用「以当前值为干点 / 湿点」，改一路不影响别人。

没校准也能用，出厂默认值（干 2800 / 湿 1150）覆盖 V2.0.0 探头的大致范围，只是精度差一些。

## 安全机制

| 机制 | 触发条件 | 作用 |
|---|---|---|
| 上电全关 | 复位瞬间 | `setup()` 第一行拉低全部泵脚；更前面的 300ms 由硬件 10k 栅极下拉兜住 |
| 单次硬上限 | 300 秒 | 无论如何强制停泵，网页配置改不动这个数 |
| 小时预算 | 每路 15 分钟/小时、全局 30 分钟/小时 | 防传感器故障导致无限浇水 |
| 断线检测 | 读数 < 400（一次即判定） | 该路禁止自动浇水，网页标红 |
| 短路检测 | 读数 > 4000（一次即判定） | 同上 |
| 卡死检测 | 连续 60 次读数一字不差 | 同上 |
| 抢占保护 | 泵运行不满 3 秒 | 不允许手动抢占，避免泵反复启停 |
| 掉电锁定 | 上次复位时泵还在转 | 冷却 5 分钟（连续两次则 30 分钟）内禁止自动浇水 |
| 看门狗 | 主循环卡死 10 秒 | 强制重启，重启后自动进入掉电锁定 |
| 采样屏蔽 | 泵启停后 300ms | 不采 ADC，避开地弹造成的假读数 |

## 远程控制（MQTT）

配置好家里路由器之后，板子默认还是在局域网里，出门在外访问不到。要真正「远程」，
最省事的是走 MQTT：板子主动连一个公共/自建 broker，手机在外网就能**看湿度、发浇水指令**，
不需要任何常开电脑、不需要公网 IP、ESP32 端也几乎不用操心重连。

**为什么选 MQTT 而不是把网页直接暴露到公网**：本设备的 HTTP 接口现在零鉴权，一旦暴露出去，
别人扫到 URL 就能开泵抽干水箱、改 WiFi 配置让你再也连不上。MQTT 把访问控制交给 broker
（账号密码 + 主题隔离），再叠加一层**指令令牌**，比裸奔网页安全得多。

### 配置（设置页 → 远程 MQTT）

| 项 | 说明 |
|---|---|
| 启用远程控制 | 打开后，板子一连上路由器就自动去连 broker |
| 服务器 / 端口 | 公共 broker 如 `broker.emqx.io:1883`，或下面的巴法云 |
| 用户名 / 密码 | 公共 broker 一般留空；巴法云填你的 uid |
| 指令令牌 | **强烈建议填**。命令必须带对令牌才执行，否则被忽略 |
| 主题前缀 | 留空自动用 `aw_XXXX`（XXXX = 芯片 ID 后四位，与热点名呼应）|

点「填入巴法云」会自动填好 `bemfa.com:9501`，再把你在巴法云拿到的 uid 填进用户名/密码即可。

### 主题与协议

| 方向 | 主题 | 内容 |
|---|---|---|
| 板子 → 你 | `aw_XXXX/state` | 每 30 秒（或状态变化）上报一次全量状态 JSON |
| 板子 → 你 | `aw_XXXX/avail` | `online`/`offline`（断线自动变 offline，靠 broker 保留）|
| 你 → 板子 | `aw_XXXX/cmd` | 下发指令，必须带令牌 |

**指令示例**（JSON，`t` 是令牌）：

```json
{"t":"你的令牌","ch":0,"a":"start","s":30}   // 通道 1 浇水 30 秒
{"t":"你的令牌","ch":0,"a":"stop"}           // 停止（正在转则停泵，排队中则撤单）
{"t":"你的令牌","ch":2,"a":"auto","on":0}    // 关闭通道 3 的自动浇水
```

### 手机端怎么用

装任意 MQTT 客户端 App（如 MQTT Dashboard、MQTT Explorer），填 broker 地址 + 账号，
订阅 `aw_XXXX/state` 看数据，往 `aw_XXXX/cmd` 发上面的 JSON 就能控制。
巴法云自带的 App 也能直接订阅主题做开关面板。

### 安全提醒

- **令牌一定不要空着**就接公共 broker，否则同 broker 上的任何人都能开你的泵。
- 主题前缀 `aw_XXXX` 和热点名同源，算不上秘密；真正挡人的是令牌 + broker 账号。
- 改 MQTT 配置**不会**触发主配置重置（它单独存 NVS 的 `mqtt` 命名空间）。
- 只有连上路由器（STA 模式）才会连 MQTT；退化到热点模式时自动断开，不空耗。

## 开发这条命令是什么用的

```bash
python tools/build_web.py      # 改了 web/index.html 之后必须跑，否则固件里还是旧页面
python tools/build_web.py --check   # 只检查是否同步
python tools/check_firmware.py      # 静态检查门禁
```

改网页的流程：编辑 `web/index.html` → 双击看效果（连不上设备时自动进入预览模式，用模拟数据）→ 跑 `build_web.py` → 重新编译烧录。

## 已知未验证项

这份固件**没有经过真实编译验证**（开发环境里没有 C++ 工具链，只有 Python 和 Node）。已经做了这些收窄风险：

- 静态检查脚本覆盖了括号配平、头文件存在性、声明/实现配对、引脚冲突、ADC2 误用、input-only 误用、生成物同步
- 只用 core 2.x 与 3.x 都存在的 API
- **完全不用 PWM**，直接消灭了 `ledcSetup` vs `ledcAttach` 这个最大的版本分歧
- 不用 `std::vector`、不用 C++17 结构化绑定、不用异常

但以下内容仍需要你本机编译一次才能确认：

1. `esp_task_wdt` 的版本分支（core 2.x 用 `init(sec, bool)`，3.x 用配置结构体）
2. `WebServer::send_P` 的 gzip 响应头是否被浏览器正确识别
3. 信号强度、AP 模式下的 DNS 劫持行为（部分安卓机连上热点后不会自动弹页，需要手动输 `192.168.4.1`）
4. 泵启停对 ADC 的实际干扰程度（软件已屏蔽 300ms，具体够不够要看实物）
5. NVS 命名空间 `water` / `watersys` 的长度限制（NVS 上限 15 字符，这两个都在范围内）

第一次编译如果报错，把报错贴给我，基本都是上面几条之一，改起来很快。

## 硬件清单

已备齐，接线看 [docs/WIRING.md](docs/WIRING.md)。核心元件：ESP32 DEVKIT V1、电容式土壤湿度传感器 ×6、5V 潜水泵 ×6、逻辑电平 N-MOS（AO3400 / IRLZ44N）×6、100Ω 与 10kΩ 电阻、1N4007、5V/2A 电源。
