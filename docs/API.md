# REST 接口

一切都在 `http://<设备IP>/` 下，同一份网页即客户端，不依赖外网。
所有响应都是 `application/json`，`ok:1` 表示成功，`ok:0` 时 `err` 给出原因。

## GET /

返回内嵌的 gzip 压缩单页（约 10KB）。带 `Content-Encoding: gzip`，浏览器自动解压，固件端零解压开销。

## GET /api/state

全量状态快照，前端每 2 秒轮询一次。约 700 字节。

```json
{
  "ok": 1,
  "fw": "1.0.0",
  "up": 84213,           // 运行秒数
  "heap": 236544,        // 可用内存
  "mode": "sta",         // sta | ap
  "ip": "192.168.1.42",
  "rssi": -58,
  "timeOk": 1,           // 时间是否已同步（每日型定时任务依赖它）
  "q": 0,                // 排队数量
  "lock": 0,             // 掉电锁定剩余秒数
  "active": { "ch": 2, "src": "timer", "remain": 18 },
  "ch": [
    { "i":0, "raw":2410, "pct":41, "flt":0, "auto":1, "thr":30,
      "run":0, "q":0, "bud":0, "dry":3100, "wet":1400 }
  ]
}
```

`active` 为 `null` 表示空闲。`flt` 故障码：0 正常 / 1 断线 / 2 短路 / 3 卡死 / 4 噪声。
`run` 正在浇水，`q` 排队中，`bud` 本小时已用秒数。

## POST /api/pump

手动浇水或停止。

```json
{ "ch": 0, "action": "start", "sec": 20 }
{ "ch": 0, "action": "stop" }
```

失败时返回 `ok:0` 与原因：

| err | 含义 |
|---|---|
| `sensor_fault` | 该通道传感器故障，请先检查接线（手动也会被拒） |
| `budget` | 超出每小时累计上限 |
| `busy` | 队列满，或当前泵刚开始运行不到 3 秒不允许抢占 |
| `cooldown` | 自动浇水的最小间隔未到 |
| `lockout` | 检测到带泵复位，系统处于冷却期 |

`sec` 会被夹到 `1~300`。手动请求优先级最高，会抢占正在跑的自动/定时任务。

## POST /api/channel

```json
{ "ch": 0, "auto": 1, "thr": 35, "maxSec": 20, "minGapSec": 300 }
```

字段都可省略，只改传了的。`thr` 夹到 5~90，`maxSec` 夹到 1~300，`minGapSec` 夹到 0~7200。

## POST /api/calibrate

把当前读数直接记为干点或湿点。返回实测原始值。

```json
{ "ch": 0, "point": "dry" }
{ "ch": 0, "point": "wet" }
```

`dry` = 探头插在干土里时按；`wet` = 泡在水里时按。固件会强制保证干点 > 湿点 + 100，否则忽略。

## POST /api/calib_all

一键把 **6 路**当前读数同时记为干点或湿点。每路连采 3 组 9 次中位数再取中位数（共 27 次 ADC），比单次采样稳。单路标定请用 `/api/calibrate`。

```json
{ "point": "dry" }
{ "point": "wet" }
```

响应：

```json
{
  "ok": 1, "point": "dry", "applied": 5,
  "items": [
    { "i": 0, "raw": 2864, "st": 0 },
    { "i": 5, "raw": 12,   "st": 1 }
  ]
}
```

- `applied`：实际写入的通道数
- `items[].st`：`0` 正常，`1` 探头未接入/断线（raw < 100），`2` 读数饱和（raw > 4000）——这两种通道会被跳过，不影响其它通道
- 写入前还会校验方向：干点必须比现有湿点高 150 以上，湿点必须比现有干点低 150 以上，否则该路跳过
- 有泵在运行时返回 `{"ok":0,"err":"busy"}`；泵刚停的 300ms 地弹窗口内会自动等待
- 成功写入后**立即落盘**（不等 2 秒去抖），并重新计算所有百分比

## GET /api/schedule

```json
{ "ok":1, "tasks":[ { "i":0, "on":1, "ch":2, "type":0,
                      "periodMin":360, "durSec":45, "hour":0, "min":0 } ] }
```

`type`：0 = 周期型（不依赖网络时间），1 = 每日时刻型（需时间已同步）。

## POST /api/schedule

新增或修改。带 `i` 表示改第 i 个槽位，不带则新建。返回分配的槽位号。

```json
{ "ch": 2, "type": "interval", "periodMin": 360, "durSec": 45, "on": 1 }
{ "i": 0, "on": 0 }
{ "ch": 1, "type": "daily", "hour": 8, "min": 0, "durSec": 30 }
```

## POST /api/schedule/del

```json
{ "i": 0 }
```

## GET /api/logs?since=N

增量拉取，`since` 传上次拿到的最大 `s`。固件日志只放 RAM（128 条环形），不写 flash。

```json
{ "ok":1, "seq":42,
  "items":[ { "s":41, "t":8421, "c":"浇水完成", "k":"ok", "ch":2 } ] }
```

`k`：`ok` / `warn` / `err` / 空。

## GET /api/scan

扫描附近 WiFi，**阻塞 2~3 秒**，前端要显示 loading。返回最多 12 个。

```json
{ "ok":1, "nets":[ { "ssid":"Home-2.4G", "rssi":-52 } ] }
```

## POST /api/wifi

保存路由器账号，2 秒后自动重启。空密码留空字符串。

```json
{ "ssid": "Home-2.4G", "pass": "12345678" }
```

## GET /api/version

```json
{ "ok":1, "fw":"1.0.0", "build":"Nov  6 2026 21:00:00", "ap":"AutoWater-3F2A" }
```

## POST /api/reboot

800ms 后重启。

## 设计取舍

- **用轮询不用 SSE**：ESP32 的 lwIP 只允许 10 个 socket，常驻连接会耗尽；且 Arduino `WebServer` 是单客户端同步处理，SSE 会长期占住处理器导致其他请求全部超时。2 秒轮询每次约 700 字节，压力可以忽略。
- **JSON 全部 `snprintf` 到栈缓冲**，不用 `String` 拼接：堆碎片是 ESP32 长时间运行崩溃的头号原因。
- **不引任何 CDN**：热点模式下没有外网，页面里一旦有外部资源引用就会白屏。
