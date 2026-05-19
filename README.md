# AirCover

> ESP32-C3 firmware that mechanically reshapes the airflow from a wall-mounted air conditioner via a soft plastic deflector, with HomeKit and a self-hosted web console.

夏天空调直吹人冷得难受，但拆装导风片麻烦又难看。AirCover 用两个 28BYJ-48 步进电机卷起一个透明塑料袋的上沿，把出风口的风形偏到任意角度——挂上去之后**再不需要碰它**：苹果"家庭" App 里一条滑块、或者打开浏览器进控制台都能用。

**Status**: ✅ 在用 · 已 HomeKit 配对成功 · OTA 远程升级可用

---

## ✨ Features

- 🏠 **HomeKit Window Covering 服务** — 在"家庭" App 显示为 0–100% 滑块，Siri 可控
- 🌐 **完整 Web 控制台** — `http://aircover.local`，含点动、标定、配置、实时日志、OTA 全功能
- 📶 **SoftAP 兜底配网** — 没存 WiFi / 连不上时自动开热点 `AirCover-Setup`，浏览器配网
- 🔄 **远程 OTA 升级** — 双 OTA 槽，Web UI 上传 `.bin`，写入备份槽校验后切换，不会变砖
- 💡 **WS2812 RGB 状态指示** — gamma 校正 + 40Hz 渲染 + 余弦呼吸 + 1s 光谱过渡
- 🧲 **位置吸附 (detent)** — 可选的离散方位角，防止双电机长期使用后位置漂移
- 🔧 **NVS 平滑迁移** — 固件升级新增配置项不会清除现有设置
- 🛡️ **掉电恢复** — 位置记忆持久化，重启后从上次位置继续

---

## 📦 Hardware

### 物料清单

| 数量 | 元件 | 说明 |
|---|---|---|
| 1 | **ESP32-C3** 开发板 | 带板载 USB-Serial-JTAG 和 WS2812 RGB LED（如 ESP32-C3-DevKitM-1、超核 C3 Mini 等） |
| 2 | **28BYJ-48** 步进电机 | 5V 版本，自带 4 线 |
| 2 | **ULN2003** 驱动板 | 跟 28BYJ-48 配套的红色小板 |
| 1 | 5V 电源 | USB 5V 即可（>500mA），不要从 ESP32 的 3V3 取 |
| - | 杜邦线、塑料袋、卷线轴 | 机械部分自由发挥 |

### 接线

| 信号 | GPIO | 接到 |
|---|---|---|
| 左电机 IN1–IN4 | **4 / 5 / 6 / 7** | ULN2003 (A) 的 IN1–IN4 |
| 右电机 IN1–IN4 | **0 / 1 / 3 / 10** | ULN2003 (B) 的 IN1–IN4 |
| BOOT 按键 | 9 (板载) | — |
| WS2812 状态 LED | 8 (板载) | — |

> ⚠️ **供电要点**：两块 ULN2003 的 5V 用外部 5V 电源（不要走 ESP32 的 3V3 上行），ESP32、两块 ULN2003、电源必须**共地**。

> 💡 **JTAG 仍可用**：虽然 GPIO 4/5/6/7 占用了传统 pin-based JTAG，但 ESP32-C3 内置 **USB-Serial-JTAG**，用 Type-C 数据线就能调试。

---

## 🚀 Quick Start

### 1. 环境准备

需要 **ESP-IDF 5.5+**。如果还没装：

```bash
# Linux/macOS
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c3 && . ./export.sh
```

Windows 用户用 [ESP-IDF Installer](https://dl.espressif.com/dl/esp-idf/) 安装即可。

### 2. 拉取 esp-homekit-sdk

本项目依赖 Espressif 的 HomeKit SDK，需要单独 clone：

```bash
cd /path/to/parent
git clone --recursive https://github.com/espressif/esp-homekit-sdk.git
```

默认会从 `../esp-homekit-sdk` 寻找。如果放别处，编译时加 `-DHOMEKIT_PATH=/your/path`。

### 3. 编译 & 烧录

```bash
git clone https://github.com/<you>/aircover.git
cd aircover
idf.py set-target esp32c3
idf.py -p /dev/ttyUSB0 flash monitor   # Windows: -p COM3
```

烧录后在串口看到：

```
I (xxx) homekit: HomeKit setup code: 111-22-333
```

记下这个 8 位数字，后面 iOS 配对要用。

> 💾 **关于 NVS 区**：首次烧录会重写整个 flash（包括 NVS），所以 WiFi、HomeKit 配对、标定**全部清空**。之后所有更新走 OTA 就不会丢配置了。

### 4. 第一次开机

1. **WiFi 配网**：上电后 LED **橙色闪烁** → 手机连 WiFi 热点 `AirCover-Setup`（无密码）→ 浏览器打开 `http://192.168.4.1` → 选择家里 WiFi → 保存
2. **加入家庭网络**：设备重启，LED 走 **boot → 蓝色闪烁(连接中) → 黄色呼吸(等待配对) → 绿色(就绪)** → 浏览器打开 `http://aircover.local`
3. **HomeKit 配对**：iPhone → "家庭" App → 添加配件 → "没有二维码？" → 手动输入 `111-22-333` → 配对完成

---

## 🎛️ Web 控制台

访问 `http://aircover.local`（或 IP 地址），会看到深色琥珀色工业风界面：

### STATUS 面板
- 实时显示位置 / 目标 / 步数 / 运动状态 / HomeKit 配对状态 / WiFi
- `FULL CLOSE` / `HALF` / `FULL OPEN` 一键到位
- 拖动滑块直接定位
- 显示两个电机各自的位置（用于检测失同步）

### JOG / CALIBRATION 面板
- 电机选择：**BOTH / LEFT / RIGHT**（用于单独微调某一边）
- 点动按钮：±10 / ±100 / ±500 步
- 自定义步数点动
- 标定按钮：
  - **SET ZERO HERE** — 当前位置设为 0%
  - **SET OPEN HERE** — 当前位置设为 100%
  - **SYNC HERE** — 强制 RIGHT 电机位置 = LEFT 电机位置（解决失同步）

### CONFIG 面板
| 字段 | 说明 |
|---|---|
| `STEP PERIOD (us)` | 每半步耗时，默认 1500。越小越快，但 28BYJ-48 低于约 1000us 会失步 |
| `HOLD WHEN IDLE` | 静止时是否保持线圈通电锁定。默认关（省电不发热）。卷线被拉力拽回去才打开 |
| `FULL-OPEN STEPS` | 满行程步数。点 SET OPEN HERE 会自动写入，也可手填 |
| `DETENT STEPS` | 位置吸附粒度（详见下文）。0 = 禁用 |
| `HOMEKIT NAME` | "家庭" App 里显示的名字 |
| `HOMEKIT SETUP CODE` | 配对码（`XXX-XX-XXX`）。改后需在"家庭"重新配对 |

### LOG 面板
- 实时镜像 ESP-IDF 控制台日志（WebSocket 推送），不用拉串口线
- `CLEAR` 清屏，`AUTOSCROLL` 控制是否跟随尾部

### FIRMWARE UPDATE 面板
OTA 远程升级，详见下方。

### DANGER 面板
- `REBOOT` — 软重启
- `FACTORY RESET` — 清空所有 NVS（WiFi、配对、标定都没了），重新走配网流程

---

## 🔄 OTA 升级

第一次以外，**所有后续更新都通过浏览器完成**，不用接 USB：

1. 改完代码：`idf.py build`
2. 浏览器打开 `http://aircover.local`
3. 滚到 **FIRMWARE UPDATE** 面板
4. 选择 `build/aircover.bin`，点 **UPLOAD & FLASH**
5. 进度条走完 → 设备自动重启 → 5 秒后页面恢复连接

**安全保护**：
- 固件写入**未激活的 OTA 槽**，当前运行的固件不动
- 校验失败不会切换 boot 分区
- 网络中断、文件损坏、空间不足，**任何环节出错都不影响当前固件**
- NVS 不动，WiFi 凭证、HomeKit 配对、标定全部保留

**新增字段不丢配置**：升级后如果 `app_settings_t` 加了新字段，启动时会从 NVS 读旧 blob、新字段保持默认，然后回写新布局。这意味着你可以放心往 settings 里加东西。

---

## 💡 LED 状态参考

板载 WS2812 (GPIO 8) 反映系统状态。所有状态之间有 1 秒颜色平滑过渡：

| 颜色 / 动作 | 含义 |
|---|---|
| 暗白常亮 | 启动中 |
| **蓝色闪烁** (0.6Hz) | 正在连 WiFi |
| **橙色快闪** (4Hz) | SoftAP 配网模式 |
| **黄色呼吸** (2s 周期) | WiFi 连上，等待 HomeKit 配对 |
| **绿色** (5s 后渐变熄灭) | 已配对、就绪 |
| **蓝色脉冲** (1.5s 周期) | 电机运动中 |

技术细节：颜色定义在感知线性空间，输出前做 gamma 2.2 编码；40Hz 渲染；脉冲/呼吸用余弦波而非三角波——为了让低亮度区也看起来流畅，没有明显的"掉帧"阶梯感。短动作（<25ms）有 latch 保证至少闪一次。

调节参数在 `main/led_indicator.c` 顶部：

```c
#define TICK_HZ                 40   // 渲染帧率
#define BLUE_PULSE_TICKS        60   // 1.5s
#define READY_VISIBLE_TICKS     (5 * TICK_HZ)
#define TRANSITION_TICKS        (1 * TICK_HZ)
// gamma 2.2 在 led_indicator_init 里
```

---

## 🧲 位置吸附 (Detent)

两个电机机械上无法完全同步，每次移动可能有零点几步的小误差。**多次运动后累积**，就会导致两边塑料袋高度不一致。

启用 detent 后，HomeKit 滑块和百分比按钮的目标位置会**吸附到整数倍的网格点**，确保每次停止都落在同一组固定位置上，误差不累积。

举例（`full_open_steps = 12288`）：

| `detent_steps` | 网格点数 | 每档约几度 |
|---|---|---|
| 0 | ∞（禁用） | — |
| 128 | 96 档 / 3 圈 | ≈ 11° |
| 256 | 48 档 / 3 圈 | ≈ 22.5° |
| 512 | 24 档 / 3 圈 | ≈ 45° |
| 1024 | 12 档 / 3 圈 | ≈ 90° |

**只影响 `move_to`，不影响 jog**——点动按钮仍然单步精度，方便微调和标定。

实测建议从 **256** 起步，太粗就降到 128，太细就提到 512。

---

## 🛠️ 配置参考

所有持久配置（`app_settings_t`）：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `wifi_ssid` / `wifi_pass` | (empty) | WiFi 凭证，SoftAP 配网时写入 |
| `full_open_steps` | 12288 | 0% → 100% 的总半步数 |
| `last_position` | 0 | 每次停止后写入，掉电恢复 |
| `step_period_us` | 1500 | 每半步耗时 |
| `hold_when_stopped` | false | 静止时是否锁定线圈 |
| `hap_setup_code` | `111-22-333` | HomeKit 配对码 |
| `accessory_name` | `AirCover` | HomeKit 配件名 |
| `detent_steps` | 0 | 吸附粒度，0 = 禁用 |

---

## 🐛 Troubleshooting

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 电机不转 / 抖动 | 5V 供电不够，或没共地 | 用独立 5V 电源，三方共地 |
| 转一会就丢步 | `step_period_us` 太小 | 改回 1500 或更大 |
| 一个电机方向反了 | 物理接反 | 在该电机引脚里交换任意两根**相邻**线（如 IN1↔IN3） |
| 两个电机不同步 | 长期误差累积 | 开 `DETENT STEPS` (从 256 试)，并周期性用 `SYNC HERE` |
| HomeKit 配对失败 | setup code 错 / mDNS 不通 / 网络隔离 | 确认代码，确认手机和设备同一 LAN，路由器允许 mDNS |
| `aircover.local` 打不开 | mDNS 不被支持 | Windows 装 Bonjour Print Services；或串口看 IP 直接访问 |
| Web UI 卡顿 / WS 掉线 | 多个客户端同时连 | 最多支持 4 个并发 WS，关掉别的标签页 |
| 长按 BOOT 没反应 | 没按够 10 秒 | 确实需要 **10 秒**（防误触） |
| OTA 上传失败 | 网络中断 / 文件不对 | 重试，文件必须是 `build/aircover.bin` |

---

## 🏗️ 架构

```
aircover/
├── CMakeLists.txt              # 顶层，注入 esp-homekit-sdk
├── partitions.csv              # NVS + otadata + ota_0/ota_1
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml       # mdns, led_strip
    ├── main.c                  # 入口、引脚映射、BOOT 长按、回调
    ├── app_settings.{h,c}      # NVS 持久化 + 平滑迁移
    ├── stepper.{h,c}           # 28BYJ-48 半步驱动（双电机独立相位）
    ├── wifi_manager.{h,c}      # STA + SoftAP 兜底
    ├── homekit_service.{h,c}   # HAP Window Covering
    ├── led_indicator.{h,c}     # WS2812 状态机
    ├── webui.{h,c}             # HTTP API + WebSocket 日志 + OTA
    └── webui_assets.h          # 控制台 HTML/CSS/JS
```

模块依赖关系：`main.c` 是唯一的全局协调者，其他模块互不引用，通过 `main.c` 注册的回调通信。

---

## 🔧 二次开发提示

- **改成单电机** — `main.c` 把 `pin_right` 设成 `{-1,-1,-1,-1}`，`stepper.c` 的 timer_cb 已经忽略 -1 引脚
- **加限位开关** — `stepper.c::timer_cb` 里读 GPIO，命中时 `m->target = m->current`
- **改成 Fan 服务** — `homekit_service.c` 用 `hap_serv_fan_create`（但失去百分比滑块）
- **加更多 NVS 字段** — 追加到 `app_settings_t` **末尾**，在 `load_defaults` 给默认值；平滑迁移会自动处理
- **改 LED 颜色** — `led_indicator.c` 顶部 `COL_*` 常量，定义在感知线性空间

---

## 📄 License

MIT —— 自由使用、修改、二次分发。仅当作 DIY 参考实现提供，不对任何空调爆炸或塑料袋着火负责。

## 🙏 Acknowledgments

- [esp-homekit-sdk](https://github.com/espressif/esp-homekit-sdk) — Espressif 官方 HAP 实现
- [ESP-IDF](https://github.com/espressif/esp-idf) — 整个 ESP32 生态
- 厨房抽油烟机柜门上那块塑料布 — 原型机灵感来源

---

如果这个项目帮到你，欢迎 ⭐ Star；遇到 bug 或想加功能，开 Issue 或 PR。
