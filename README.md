# claude-desktop-buddy-StickS3

M5Stack StickS3 移植版，基于官方参考固件 [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)。

Claude Desktop（macOS / Windows，开启 Developer Mode 后）可以通过 BLE 连接"硬件伙伴"设备，把会话状态、权限审批提示等推到小屏幕上。上游参考固件的目标是 M5StickC Plus（ESP32-PICO + AXP192），本仓库把它移植到 **M5Stack StickS3**（ESP32-S3 + M5PM1 + BMI270 + 210mAh）。

> 想自己从零做一台？不需要本仓库代码。BLE 协议规范看 **[REFERENCE.md](REFERENCE.md)**：Nordic UART Service UUID、JSON schema、文件夹推送协议。

<p align="center">
  <img src="docs/device.jpg" alt="M5Stack StickS3 running the buddy firmware" width="500">
</p>

---

## 移植工作清单

### 1. 硬件 / HAL

- 目标板：M5StickC Plus → **M5Stack StickS3**
- HAL：`m5stack/M5StickCPlus` → **`m5stack/M5Unified`**
- PlatformIO 平台：`espressif32` → **`pioarduino/platform-espressif32#54.03.20`**（ESP32-S3 支持）
- CPU 240MHz / 8MB flash / LittleFS / `no_ota.csv` 分区
- `ARDUINO_USB_MODE=1 + ARDUINO_USB_CDC_ON_BOOT=1`：USB 走 USB-Serial-JTAG 块，跟 GPIO19 LED 让路

### 2. BLE 协议栈：Bluedroid → NimBLE

从 Arduino-ESP32 自带的 Bluedroid 换成 `h2zero/NimBLE-Arduino @ ^2.2.0`。

- connected idle 功耗约 **-30~50%**
- RAM / flash 占用明显更小
- 原版安全等级**等价迁移**：LE Secure Connections + MITM + bonding，IO capability DisplayOnly，两个 NUS 特性用 `READ_AUTHEN` / `WRITE_AUTHEN` 强制加密链路

### 3. ⭐ 关键坑：macOS BLE 名字缓存导致 Claude Desktop 扫不到设备

**这个问题值得单独一章，因为调试过程最曲折，且任何人烧过其他固件再刷本固件的 M5StickS3 都会踩。**

#### 现象

- LightBlue（macOS）：能看到一个设备，但名字显示成另一个固件（比如 `QuickVibeStick-S3`），Service UUID 却是我们固件的 NUS `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- nRF Connect（手机，没缓存过这块板子）：能看到正确的 `Claude-XXXX`
- **Claude Desktop：扫不到设备，无法连接**
- `sudo pkill bluetoothd` 无效，缓存仍在
- `/Library/Preferences/com.apple.Bluetooth.plist` 里翻不到相关条目

#### 根因

macOS 的 `bluetoothd` 按 BLE 外设的 **Public MAC 地址** 缓存设备名。缓存持久化到 SIP 保护的磁盘位置（`/Library/Bluetooth/` 或类似），用户空间看不到也直接清不掉。

这块板子之前烧过 [QuickVibeStickS3](https://github.com/) 固件并跟 Mac 连过，于是：

1. `bluetoothd` 永久记录：`MAC=XX:XX:XX:XX:XX:XX → name="QuickVibeStick-S3"`
2. 之后无论这个 MAC 广播什么名字，macOS 的 `CBPeripheral.name` 都返回缓存值
3. LightBlue 用 `CBPeripheral.name` 显示 → 看到旧名字
4. Claude Desktop 按名字前缀 `Claude` 过滤（[REFERENCE.md:31](REFERENCE.md#L31)） → 直接过滤掉，扫不到
5. 但 `CBAdvertisementDataLocalNameKey`（广播包里的 raw local name AD）其实是 `Claude-XXXX` —— 手机没缓存所以能正确显示

#### 定位过程（走过的弯路）

1. 怀疑 NimBLE 广播包不合规 → 打开 `CORE_DEBUG_LEVEL=4`，串口日志显示 `setAdvertisementData`/`setScanResponseData` 字节完全正确
2. 怀疑 31 字节广告包溢出 → 加 `enableScanResponse(true) + setName()` 把名字放进 scan response，LightBlue 里显示出 `Local Name: Claude-5AF5`，但**整体条目名依旧是 `QuickVibeStick-S3`**，Claude Desktop 还是扫不到
3. 用**从没连过这块板子的手机**装 nRF Connect 扫 → 看到正确的 `Claude-5AF5`，证明固件没问题
4. 结论：**macOS 按 public MAC 缓存了名字**

#### 解法

让设备用 **BLE Static Random Address**，macOS 按新地址当新设备，没有历史缓存可查。

```cpp
// src/ble_bridge.cpp
uint8_t mac[6] = {0};
esp_read_mac(mac, ESP_MAC_BT);
// NimBLE 期望 little-endian；把 public MAC 反序，然后强制最高 2 bit 为 11
// 满足 BT Core Spec 对 Static Random Address 的约束。
uint8_t addr[6] = { mac[5], mac[4], mac[3], mac[2], mac[1], mac[0] };
addr[5] |= 0xC0;
NimBLEDevice::setOwnAddr(addr);
NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
```

- **从 eFuse BT MAC 派生**：同一块板子每次启动得到相同的 Random Address → 配对一次 bond 就能持续自动重连
- **与 public MAC 不同**：绕开 macOS 的旧名字缓存，Claude Desktop 能正确看到 `Claude-XXXX`

配套还显式把 Local Name 放到 scan response（主广告包装不下 flags + 128-bit NUS UUID + 名字，31 字节会溢出）：

```cpp
adv->enableScanResponse(true);     // 必须先调，否则 setName 会塞进主广告包
adv->addServiceUUID(svc->getUUID());
adv->setName(deviceName);          // NimBLE 2.x 自动路由到 scan response
```

#### 触发条件与影响

- 任何人**首次**刷这个固件到**没烧过其他 BLE 固件**的 StickS3：不触发此坑
- 同一台板子刷过 QuickVibe / 其他自定义固件再刷本固件：触发，但我们的 Random Address 直接绕开，用户无感知
- 同一台 Mac 连过多个不同设备：不影响，每个设备有自己的 MAC→name 记录

### 4. 功耗优化

StickS3 屏 135×240 相比 StickC Plus 80×160 像素多 2.5 倍，背光是耗电大头。几项针对性优化：

| 项 | 原版 | 本版 | 影响 |
|---|---|---|---|
| 默认亮度 | 4/4 | **2/4** | 背光 -40% |
| nap（面朝下）CPU | 240MHz | **80MHz** | nap 期 CPU 动态功耗 -66% |
| screen off 等待 | `delay(100)` | **`esp_light_sleep_start()`** 100ms | idle 整机从 ~40mA 降到 ~5mA，BLE 链接保持 |
| BLE 广播间隔 | ~30ms（Bluedroid 默认） | **500-800ms** | 未连接时 adv 功耗 -20× |
| IMU | MPU6886 ~3.8mA | BMI270 ~420µA（硬件差异） | 持续 -3.4mA |

活跃续航跟原版基本持平（背光涨幅被 210mAh 电池和上述优化抵消）。

### 5. StickS3 硬件差异说明

- **无硬件 RTC**：断电后时间丢失。`tokens_today` 要等桌面端在当天推过 `{"time":[...]}` 才会正常滚动
- **电池电流 `bat.mA` 是哨兵值**：M5PM1 没有电流寄存器。充电中置 `-1`，放电时按 REFERENCE.md 的 "omit fields you don't have" 省略。桌面端 `mA < 0` 仍渲染为充电
- **电源键只有两态**：短按切屏，长按 6s 硬关机（无 AXP192 的多击）
- **LED = GPIO19（active-low）**：跟 USB D+ 共用物理引脚，靠 `ARDUINO_USB_MODE=1` 切 USB-Serial-JTAG 块避开
- **芯片温度 ±10°C**：读 ESP32-S3 内置传感器（不是 PMIC 温度），仅供参考
- **Info → Credits 的 source URL** 已改为本仓库地址

---

## 编译烧录

需要 [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)。

```bash
pio run -t upload
```

彻底擦除再刷：

```bash
pio run -t erase && pio run -t upload
```

或设备端出厂重置：按住 A → Settings → Reset → Factory reset → 点两次。

---

## 配对

1. Claude Desktop：**Help → Troubleshooting → Enable Developer Mode**
2. **Developer → Open Hardware Buddy…** → **Connect**
3. 从列表选 `Claude-XXXX`（XXXX 是 Random Static Address 末两字节，与 BT MAC 不同）
4. macOS 首次连会弹出配对框，设备屏幕会显示 6 位 passkey → 在 Mac 上输入
5. 配对成功，bond 存到设备 NVS，后续自动加密重连

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy…" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window" width="420">
</p>

### Claude Desktop 扫不到？

本固件用的 Static Random Address 已经绕开最常见的 macOS 缓存问题。如果仍然扫不到，按顺序排查：

1. 设备醒着吗？任意按键唤醒屏幕
2. 用手机 nRF Connect 扫一下，应该能看到 `Claude-XXXX`。如果**手机都扫不到**，固件端有问题 —— 抓串口日志（`pio device monitor -b 115200`，先把 `CORE_DEBUG_LEVEL` 临时改回 4）
3. 如果手机能扫到、Mac 扫不到 → System Settings → Bluetooth 里翻一遍，Forget 任何可疑旧条目
4. 实在不行：Option-点菜单栏蓝牙图标 → Reset the Bluetooth module

---

## 控制

|                         | Normal               | Pet         | Info        | Approval    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A**（前面）           | 下一屏                | 下一屏       | 下一屏       | **approve** |
| **B**（右侧）           | 滚动 transcript       | 下一页       | 下一页       | **deny**    |
| **Hold A**              | 菜单                  | 菜单         | 菜单         | 菜单         |
| **电源**（左侧短按）    | 切屏                  |              |              |              |
| **电源**（左侧长按 6s） | 硬关机                |              |              |              |
| **摇动**                | dizzy                |              |              | —            |
| **朝下**                | nap（回能量、降频）   |              |              |              |

30 秒无交互后自动关屏（审批过程中保持常亮）；任意按键唤醒。

---

## ASCII 宠物

18 种，每种 7 组动画（sleep / idle / busy / attention / celebrate / dizzy / heart）。Menu → "next pet" 切换，NVS 持久化。

## GIF 宠物

往 Hardware Buddy 窗口的投放区拖一个字符包文件夹，桌面端通过 BLE 推到设备，自动切到 GIF 模式。**Settings → delete char** 回 ASCII。

字符包结构：`manifest.json` + 一堆 96px 宽的 GIF：

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

- state 值可以是单个文件名或数组；数组会每次动画循环结束切下一个
- GIF 宽 96px，高 ≤140px 能塞进 135×240 竖屏
- 整个文件夹 ≤1.8MB；`gifsicle --lossy=80 -O3 --colors 64` 通常能压掉 40-60%
- `tools/prep_character.py` 负责对齐尺寸
- `tools/flash_character.py characters/bufo` 跳过 BLE，直接走 USB 把 `data/` 刷进去

参考 `characters/bufo/`。

## 七种状态

| 状态        | 触发                       | 感觉                       |
| ----------- | -------------------------- | -------------------------- |
| `sleep`     | 桥未连接                    | 闭眼呼吸                   |
| `idle`      | 已连接，无事                | 眨眼、环顾                 |
| `busy`      | 会话在跑                    | 流汗、工作                 |
| `attention` | 有待审批                    | 警觉、**LED 闪烁**         |
| `celebrate` | 升级（每 50K tokens）       | 撒花、跳动                 |
| `dizzy`     | 摇了设备                    | 眼睛转圈                   |
| `heart`     | 5 秒内批准                  | 飘爱心                     |

---

## 项目结构

```
src/
  main.cpp        ─ 主循环、状态机、UI、电源管理
  buddy.cpp       ─ ASCII 物种分发
  buddies/        ─ 每个物种一个文件，7 个动画函数
  ble_bridge.cpp  ─ NUS 桥（含 Static Random Address 派生）
  character.cpp   ─ GIF 解码渲染
  data.h          ─ 协议解析
  xfer.h          ─ 文件夹推送接收器
  stats.h         ─ NVS 持久化（stats / settings / owner / 物种选择）
characters/       ─ 示例 GIF 包
tools/            ─ 转换 / 辅助脚本
```

---

## 可用性

BLE API 仅在桌面端开启 Developer Mode 后可用（**Help → Troubleshooting → Enable Developer Mode**），面向 maker / 开发者，非官方正式功能。
