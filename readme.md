# SmartHouse ESP32-CAM 智能家居摄像头

基于 AI-Thinker ESP32-CAM 的局域网网络摄像头，带 Web UI，并作为 **TCP 客户端**
自动发现手机 APP、上报自己的访问网址。

- 板子：AI-Thinker ESP32-CAM
- 核心：esp32 by Espressif 2.0.7（Arduino IDE）
- 串口：115200

## 功能

| 地址 | 说明 |
| --- | --- |
| `http://<esp-ip>/` | Web 管理界面 |
| `http://<esp-ip>/stream` | MJPEG 实时流 |
| `http://<esp-ip>/capture` | 单张 JPEG |
| `http://<esp-ip>/led?state=on\|off\|toggle` | 补光灯 (GPIO4) |
| `http://<esp-ip>/control?var=<name>&val=<v>` | 摄像头参数 |
| `http://<esp-ip>/status` | 状态 JSON |

## 手机 APP 自动发现 + 上报

手机 APP 的 IP 无法预先写死，所以 ESP32 主动广播发现，发现后以 TCP 客户端
连上 APP，并把自己的 `http://<esp-ip>/` 上报过去。

```
ESP32 (TCP 客户端)                        手机 APP (TCP 服务端 + UDP 监听)
      │  UDP 广播 :4210
      │ ─────────────────────────────────────►
      │  SMARTHOUSE_DISCOVER:ESP32CAM\n
      │  ◄─────────────────────────────────────
      │  SMARTHOUSE_SERVER:4211\n        (端口可省略，默认 4211)
      │  TCP 连接 :4211
      │ ─────────────────────────────────────►
      │  {"type":"register",...}\n
      │  {"type":"heartbeat",...}\n     每 10s
```

### 协议

所有消息以 `\n` 结尾。

1. ESP32 → APP（UDP 广播 `255.255.255.255:4210` 与子网广播）
   ```
   SMARTHOUSE_DISCOVER:ESP32CAM
   ```
2. APP → ESP32（UDP 单播回同一端口，端口可选）
   ```
   SMARTHOUSE_SERVER:4211
   ```
3. ESP32 → APP（TCP `:4211`），连接成功后立即发送
   ```json
   {"type":"register","name":"ESP32-CAM","ip":"10.182.44.37","port":80,"url":"http://10.182.44.37/","rssi":-52}
   ```
4. ESP32 → APP 心跳，每 10 秒
   ```json
   {"type":"heartbeat","ip":"10.182.44.37","port":80,"url":"http://10.182.44.37/","rssi":-52}
   ```

APP 可以回发任意以 `\n` 结尾的指令，ESP32 会打印到串口（可在
`appReadFromServer()` 中扩展处理）。

### 兜底扫描

如果 APP 只做 TCP 服务端、不回应 UDP，ESP32 会在启动发现 15 秒后，
逐个扫描本机 `/24` 网段上的 `4211` 端口，找到后在 `appScanStep()` 中连接。
可用 `APP_SCAN_FALLBACK` 宏关闭。

## 可调参数

在 `smarthouse.ino` 的「PHONE APP LINK」段落：

```cpp
#define APP_DISCOVERY_PORT   4210    // UDP 发现端口（两端一致）
#define APP_TCP_PORT         4211    // APP 默认 TCP 端口
#define APP_DISCOVERY_MS     3000    // 广播间隔
#define APP_HEARTBEAT_MS     10000   // 心跳间隔
#define APP_CONNECT_TIMEOUT  1500    // TCP 连接超时
#define APP_RETRY_MS         3000    // 重连间隔
#define APP_RETRY_LIMIT      3       // 连续失败几次后重新发现
#define APP_SCAN_FALLBACK    1       // 是否启用 /24 扫描兜底
```

## APP 端需要做什么

1. 监听 UDP `4210`，收到含 `SMARTHOUSE_DISCOVER` 的包后，向来源地址回
   `SMARTHOUSE_SERVER:<tcp端口>\n`。
2. 启动 TCP 服务端（默认 `4211`），接受 ESP32 连接。
3. 读取每条 `\n` 结尾的 JSON，从中取 `url` 字段即可访问摄像头页面。

## 注意

- WiFi 账号密码在 `smarthouse.ino` 顶部的 `USER CONFIG` 段落，
  上传仓库前请改成占位符。
- 手机与 ESP32 必须在同一局域网，且路由器未开启 AP 隔离
  （否则 UDP 广播与 TCP 直连都会被拦截）。
- 若首次 WiFi 连接超时，固件仍会启动 Web 服务器与 APP 客户端，
  网络恢复后自动可用。
