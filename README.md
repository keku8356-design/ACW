# Aircover

ESP32-C3 驱动两个 28BYJ-48 步进电机（配 ULN2003）卷线，把塑料袋导风口的上边缘拉起或放下，引导空调出风。**接入 HomeKit**，同时提供一个**全功能远程 Web 控制面板**用于调试、标定、配置——挂上空调后再不需要拆下来碰它。

> ESP-IDF 5.5+ &nbsp;·&nbsp; HomeKit Window Covering 服务 &nbsp;·&nbsp; SoftAP 兜底配网 &nbsp;·&nbsp; WebSocket 实时日志

---

## 1. 硬件接线

| 信号 | 引脚 | 说明 |
|---|---|---|
| 电机 A IN1..IN4 | GPIO **0 / 1 / 3 / 4** | 接 ULN2003 板 A 的 IN1..IN4 |
| 电机 B IN1..IN4 | GPIO **5 / 6 / 7 / 10** | 接 ULN2003 板 B 的 IN1..IN4 |
| BOOT 按键 | GPIO 9（板载） | 长按 **10 秒**触发恢复出厂 |
| 状态 LED | GPIO 8（板载） | 上电常亮 → 慢闪连 WiFi → 快闪等待配网 → 灭代表 idle |

**供电**：
- ULN2003 + 28BYJ-48 用 **5V**（USB 5V 即可，但不要从 ESP32-C3 的 3V3 取电）
- ESP32-C3、两块 ULN2003 板、5V 电源**共地必须**
- 两块 ULN2003 板的 5V 各自接一起即可

**关于丢失的功能**：电机用 GPIO 4/5/6/7 占用了 pin-based JTAG，但 ESP32-C3 的 **USB-Serial-JTAG**（type-C 数据线即可）仍可正常 JTAG 调试，不受影响。

---

## 2. 编译

### 2.1 拉取 esp-homekit-sdk

在 `aircover` 的**父目录**执行：
```bash
git clone --recursive https://github.com/espressif/esp-homekit-sdk.git
```

最终结构：
```
your-workspace/
├── aircover/             # 本项目
└── esp-homekit-sdk/      # 与本项目并列
```

如要放别处，编译时用 `-DHOMEKIT_PATH=/abs/path/to/esp-homekit-sdk`。

### 2.2 设置目标芯片并编译

```bash
cd aircover
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

> 烧录后 `monitor` 输出里会打印 `HomeKit setup code: 111-22-333`，**第一次配对**就用这个 8 位数字（可在 Web UI 改）。

---

## 3. 首次使用流程

1. **上电** → LED 慢闪。设备没有保存的 WiFi，60 秒后自动进入 SoftAP 模式（LED 改为快闪）。
2. 手机连接 WiFi 热点 `AirCover-Setup`（开放，无密码）。
3. 浏览器访问 `http://192.168.4.1`（多数手机会自动弹出配网页）。
4. 选择家里的 WiFi，输入密码，**SAVE & REBOOT**。
5. 设备重启，加入家庭 WiFi 后：
   - LED 熄灭
   - `http://aircover.local` 进入控制面板
   - **iPhone 打开"家庭"App → 添加配件 → 没有二维码？手动输入** → 输入 `111-22-333`

之后只要 WiFi 没换，每次上电直接连进来。**换 WiFi 的话**：要么远程登录控制面板按"FACTORY RESET"，要么物理长按 BOOT 10 秒；之后会再回到 SoftAP 配网。

---

## 4. 标定流程

设备出厂默认假定 `12288 步`（≈3 圈）= 完全打开。**真实行程必须实地标定一次**：

1. 装上电机和卷线，但**先不要绑死**塑料袋上边缘（防止标错导致拉爆）
2. 浏览器进入 `http://aircover.local`
3. 在 **JOG / CALIBRATION** 面板，用 `-100` / `+100` 按钮把卷轴**转到"塑料袋完全闭合"位置**
4. 点击 **SET ZERO HERE** → 当前位置成为 0%
5. 继续点 `+100` 把卷轴**转到"塑料袋完全打开"位置**
6. 点击 **SET OPEN HERE** → 当前步数成为 100%

之后 HomeKit 滑块的 0%/100%、网页上的位置百分比都基于这个标定。

> 想重新标定：随时再走一遍即可。`full_open_steps` 也可以在 CONFIG 面板里直接写入数字。

---

## 5. Web 控制面板功能

访问 `http://aircover.local`（或 IP）：

- **STATUS**：实时显示位置 / 目标 / 步数 / 是否运动；点击 `FULL CLOSE` / `HALF` / `FULL OPEN` 或拖动滑块直接移动
- **JOG / CALIBRATION**：±10 / ±100 / ±500 步点动，自定义步数，零点和全开标定
- **CONFIG**：
  - `STEP PERIOD (us)`：每个半步的时间，默认 1500us。值越小越快。**低于 ~1000us 28BYJ-48 会失步**
  - `HOLD WHEN IDLE`：到位后是否保持线圈通电锁定。默认关（省电、不发热）。如果塑料袋拉力把卷轴往回拽，再打开
  - `FULL-OPEN STEPS`：直接输入校准后的步数
  - `HOMEKIT NAME` / `SETUP CODE`：改了之后需要在"家庭"App 里**重新配对**
- **LOG**：实时镜像 ESP-IDF 串口日志（WebSocket 推送），不需要 USB 调试线
- **DANGER**：恢复出厂 / 重启

---

## 6. HomeKit 行为细节

- 服务类型：**Window Covering**（在"家庭"里显示为带 0–100% 滑块的窗帘）
- 收到目标位置后立即开始移动，`Position State` 设为 `Increasing` / `Decreasing`
- 到位后 `Current Position = Target`，`Position State = Stopped`
- 当前位置写入 NVS，掉电后恢复
- 同一时刻新的目标会覆盖旧目标（线性平滑过渡需要中间态推送，这里没做——28BYJ-48 速度本来就不快，体验也够）

---

## 7. 故障排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 电机不转 / 抖动 | 5V 供电不足，或 ULN2003 没共地 | 用独立 5V 适配器，确认 GND 三方共地 |
| 转一两秒就失步 | `step_period_us` 太小 | 改回 1500us 或更大 |
| 一个电机方向反了 | 物理装反 | 把那个电机的两根**相邻** IN 线互换（例如 IN1↔IN3 或 IN2↔IN4） |
| iPhone 配对失败 | setup code 错或 mDNS 不通 | 确认 setup code，确认手机和设备在同一 LAN，路由器没禁用 mDNS |
| `aircover.local` 打不开 | 路由器/系统不支持 mDNS | 用串口看分配到的 IP，直接 `http://<ip>` |
| Web UI 进不去 | WiFi 没连上 | 进 SoftAP 模式（长按 BOOT 10s）重新配网 |

---

## 8. 文件结构

```
aircover/
├── CMakeLists.txt              # 顶层，注入 esp-homekit-sdk 组件路径
├── partitions.csv              # NVS + factory（2MB app）
├── sdkconfig.defaults          # ESP-IDF 全项目默认设置
└── main/
    ├── CMakeLists.txt          # 组件级
    ├── idf_component.yml       # mdns / json 依赖
    ├── main.c                  # 入口、引脚映射、状态机
    ├── app_settings.{h,c}      # NVS 持久化（WiFi / 标定 / 参数）
    ├── stepper.{h,c}           # 28BYJ-48 半步驱动，双电机同步
    ├── wifi_manager.{h,c}      # STA + SoftAP 配网兜底
    ├── homekit_service.{h,c}   # Window Covering 服务
    ├── webui.{h,c}             # HTTP API + WebSocket 日志
    └── webui_assets.h          # 控制面板 HTML/CSS/JS
```

---

## 9. 改造提示

- **改电机数 / 方向**：编辑 `main.c` 顶部的 `PIN_MOTOR_*` 宏；如要让 B 反转，交换它的两根相邻 IN 引脚即可
- **改成风扇语义**：在 `homekit_service.c` 把 `hap_serv_window_covering_create` 换成 `hap_serv_fan_create`（但失去 0-100% 滑块体验）
- **加限位开关**：在 `stepper.c` 的 `timer_cb` 里检查 GPIO，命中时 `s_target = s_current`
- **加 OTA**：`partitions.csv` 已经给 factory 留了 2MB，要 OTA 改成双 ota 分区 + 加 `esp_https_ota` 调用

祝玩得开心！
