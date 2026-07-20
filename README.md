# 语音聊天机器人固件 · 闯关开发指南

> 配套方案文档：`../voice-chatbot-architecture.md`
> 运行环境：VSCode + ESP-IDF 扩展（**ESP-IDF v6.0.2**），芯片 ESP32-S3
> ✅ **硬件实情（2026-07-13 实测）**：标称 N16R8，**实测确认带 8MB Octal PSRAM**（早期误判为无
> PSRAM 是 sdkconfig 残留 TYPE_QUAD 所致，删 sdkconfig + 选 Octal(OPI) 重配后识别成功：日志见
> `esp_psram: Found 8MB PSRAM device`）。离线大模型 / multinet 等依赖 PSRAM 的能力均可用，无需降级。

## 关卡地图（Quest Map）

| 关卡 | 目标 | 产出 | 状态 |
|------|------|------|------|
| **L0** | 环境自检 | 确认 IDF 版本 / 板子 / 串口 | ✅ |
| **L1** | 项目骨架 + LED 心跳 | FreeRTOS 双任务跑通 | ✅ 通关 |
| **L2** | 麦克风采集 (INMP441 I2S IN) | 读 PCM + 计算 RMS 音量 | ✅ 通关 |
| **L3** | 喇叭播放 (MAX98357A I2S OUT) | 播放测试音 | ✅ 通关 |
| **L4** | OLED 显示 (SSD1306 I2C) | 状态/音量/RSSI | ✅ 通关 |
| **L5** | 按键 + 状态机骨架 | IDLE/LISTENING/... 态切换 | 🔵 当前关 |
| **L6** | WiFi + NVS + SoftAP 配网 | 配网持久化 | ⏳ |
| **L7** | WebSocket 通信 + Opus 框架 | 上行音频/下行文本 | ⏳ |
| **L8** | 唤醒词 (wakenet) | 离线唤醒 + 打断 | ⏳ |
| **L9** | 离线兜底 (multinet) | 断网本地命令模式 | ⏳ |
| **L10** | 调试台 + OTA | Web 遥测 / 固件升级 | ⏳ |
| **L11** | 全链路联调 | 完整对话跑通 | ⏳ |

---

## L2 · INMP441 麦克风 I2S 采集（本关交付）

在 L1 基础上叠加 I2S 数字麦克风采集，验证音频通路。

### 接线表（INMP441 → ESP32-S3）

| INMP441 引脚 | ESP32-S3 | 说明 |
|--------------|----------|------|
| VDD          | 3.3V     | **必须 3.3V，切勿接 5V** |
| GND          | GND      | |
| SCK          | GPIO5    | 位时钟 BCLK（ESP 输出） |
| WS           | GPIO4    | 声道选择 LRC（ESP 输出） |
| SD           | GPIO6    | 数据输出 → ESP32 DIN |
| L/R          | GND      | 接地 = 选择左声道 |
| CHIPEN       | 3.3V     | 使能脚，可直接接 VDD |

> 接线时先断电；接好后再上电/烧录。INMP441 是纯输入器件，ESP32 这侧 I2S 配置为
> **RX（主模式）**，由 ESP32 输出 BCLK/WS，麦克风把数据发到 DIN。

### 编译 / 烧录 / 监视

```bash
idf.py build
idf.py -p COM3 flash monitor     # COM3 换成你的端口
```

（首次或改过 sdkconfig.defaults 后先 `idf.py reconfigure`）

### ✅ 验收标准（通关条件）

1. 监视器启动后应看到：
   ```
   I (xxx) mic: I2S 麦克风已启动: 16000 Hz, 16bit, MONO
   ```
2. **对着麦克风正常说话**，日志出现有声音行，且 RMS 数值**明显高于安静时**：
   ```
   I (xxx) mic: mic RMS=1234 (有声音)
   ```
3. 安静时基本不打印（阈值过滤），说话时打印——说明采集与音量计算都正常。
4. LED 仍每 1 秒闪一次（L1 功能未丢）。

> 若完全没打印 / RMS 恒为 0：检查 SCK/WS/SD 三根线是否接错或虚焊；确认 L/R 接地。
> 若报 I2S 引脚冲突：换一组空闲 GPIO（避开 26–37 的 PSRAM 区、43/44 的 UART）并重编。

### 🐞 常见坑

- **I2S 初始化 `ESP_ERR_INVALID_ARG`**：GPIO 选了被占用的脚（如 43/44 是日志串口），换 5/4/6 这类空闲 GPIO（避开 26–37 的 PSRAM 区）。
- **只有噪声、RMS 一直很大**：麦克风没供到 3.3V，或 VDD/GND 接触不良；检查 CHIPEN 是否使能。
- **采样率不对 / 声音变调**：本代码固定 16kHz，后续服务器 ASR 也要 16kHz，保持一致。

---

---

## L3 · MAX98357A 喇叭 I2S 播放（本关交付）

在 L2 基础上叠加 I2S 数字功放播放，验证"出声"通路。MAX98357A 是**数字功放**，直接吃 I2S 数字
信号（内部自带 DAC），无需外接 DAC 芯片。它与麦克风是"对称"的：麦克风用 I2S **接收(RX)**，喇叭
用 **发送(TX)**；两者分别用 I2S0 / I2S1 两个控制器，互不抢占。

### 接线表（MAX98357A → ESP32-S3）

| MAX98357A 引脚 | ESP32-S3 | 说明 |
|----------------|----------|------|
| VDD            | 3.3V     | **必须 3.3V，切勿接 5V** |
| GND            | GND      | |
| BCLK           | GPIO15   | 位时钟（ESP 输出） |
| WSEL / LRC     | GPIO16   | 声道选择（ESP 输出） |
| DIN            | GPIO7    | 数据输入（ESP 输出到功放） |
| GAIN           | GND      | 接地 = 9dB 增益（也可悬空 = 12dB / 接 VDD = 15dB） |
| SD（关断）     | 3.3V     | 拉高 = 常使能（接 VDD 即可一直发声；若需静音控制将来可改接 GPIO） |

> 接线时先断电。ESP32 这侧 I2S 配置为 **TX（主模式）**，由 ESP32 输出 BCLK/WS，把数据发到 DIN。
> 注意 BCLK/WS/DIN 要与 `audio_out.h` 里的宏一致（喇叭：**BCLK=GPIO15 / WS=GPIO16 / DOUT=GPIO7**；
> 麦克风是 GPIO5/4/6，互不冲突）。

### 编译 / 烧录 / 监视

```bash
idf.py build
idf.py -p COM3 flash monitor     # COM3 换成你的端口
```

### ✅ 验收标准（通关条件）

1. 启动日志应看到：
   ```
   I (xxx) audio_out: I2S 喇叭已启动: 16000 Hz, 16bit, MONO
   ```
2. **喇叭周期性发出"嘀——"的 1kHz 测试音**（每 3 秒一次，每次约 1 秒）。
3. LED 仍每秒闪、麦克风仍在采集（L1/L2 功能未丢）。
4. 监视器出现：
   ```
   I (xxx) audio_out: 播放测试音 1000 Hz, 1000 ms 完成
   ```

### 🐞 常见坑

- **完全没声音**：① 确认 VDD/GND 供到 3.3V，SD 拉到 3.3V（使能）；② 把 `audio_out.c` 里
  `I2S_SLOT_MODE_MONO` 改成 `I2S_SLOT_MODE_STEREO` 再编译（少数功放按立体声帧解析）；
  ③ 检查 BCLK/WSEL/DIN 三根线是否接错针脚。
- **有"啪"一声但不连续 / 爆音**：正弦波振幅 30000 已留余量；若仍爆音可调小（如 20000）或检查功放供电。
- **引脚冲突 `ESP_ERR_INVALID_ARG`**：换空闲 GPIO（避开 26–37 PSRAM 区、43/44 UART），并同步改
  `audio_out.h` 里的 SPK_BCLK_GPIO / SPK_WS_GPIO / SPK_DOUT_GPIO。

---

---

## L4 · SSD1306 OLED 显示（I2C，本关交付）

在 L3 基础上叠加一块 0.96 寸 OLED（SSD1306 驱动，I2C 接口），用来显示状态（后面状态机会
频繁用到）。ESP-IDF 自带 `esp_lcd` 组件里就有现成的 SSD1306 驱动，我们用三步走：
**I2C 总线 → panel IO(I2C) → SSD1306 面板**，再往自己的帧缓冲里画字、刷屏即可。

### 接线表（SSD1306 OLED → ESP32-S3）

| OLED 引脚 | ESP32-S3 | 说明 |
|-----------|----------|------|
| VCC       | 3.3V / 5V | 看模块标注；多数 0.96 寸屏 VCC 接 **3.3V** 即可（标 5V 的接 5V） |
| GND       | GND      | |
| SDA       | GPIO41   | I2C 数据线（ESP32-S3 默认 JTAG-TDI，当 GPIO 用没问题） |
| SCL       | GPIO42   | I2C 时钟线（ESP32-S3 默认 JTAG-TDO，当 GPIO 用没问题） |

> 接线时先断电。SDA/SCL 要接**上拉电阻**（模块上通常已自带 4.7k，软件里也开了内部上拉，双保险）。
> 引脚在 `oled.c` 顶部宏里：`OLED_I2C_SDA_GPIO=41` / `OLED_I2C_SCL_GPIO=42`；屏地址 `OLED_ADDR=0x3C`
> （极少数模块是 0x3D，若屏不亮把它改成 0x3D 再编）。屏幕若是 128x32，把 `OLED_HEIGHT` 改成 32。

### 编译 / 烧录 / 监视

```bash
idf.py build
idf.py -p COM3 flash monitor     # COM3 换成你的端口
```

### 这一关新增了什么

- `main/oled.h` + `main/oled.c`：OLED 模块（初始化 + 8x8 字体 + 帧缓冲 + 演示任务）。
- `main/CMakeLists.txt` 的 `REQUIRES` 加了 `esp_lcd`（SSD1306 驱动所在组件）和 `esp_driver_i2c`（I2C 主总线）。
- `main.c` 里加了 `oled_init()` + 创建 `oled_task`。
- `oled_task` 会每 2 秒切换 `BOOT / IDLE / LISTEN / THINK / SPEAK`，方便肉眼验收屏幕在动。
  （该自动轮播是 L4 的临时演示，已在 L5 被状态机的事件驱动取代：`oled_task` 已移除。）

### ✅ 验收标准（通关条件）

1. 启动日志看到：
   ```
   I (xxx) oled: OLED 已初始化 128x64 @ I2C 0x3C (SDA=GPIO41, SCL=GPIO42)
   ```
2. **屏幕每 2 秒切换一次状态**，依次显示 `STATE: BOOT` → `STATE: IDLE` → `STATE: LISTEN`
   → `STATE: THINK` → `STATE: SPEAK`，然后循环。
3. 同时 LED 仍闪、麦克风仍在采集、喇叭仍"嘀"（L1/L2/L3 功能都没丢）。

### 🐞 常见坑

- **屏幕完全不亮**：① 先确认 VCC 接对（3.3V 还是 5V 看模块丝印）；② 把 `oled.c` 里
  `OLED_ADDR` 从 `0x3C` 改成 `0x3D` 再编；③ 检查 SDA/SCL 有没有接反或虚焊。
- **花屏 / 乱码**：便宜克隆屏 I2C 抗干扰差，把 `oled.c` 里 `OLED_I2C_FREQ_HZ` 从 `400000`
  降到 `100000`（100kHz）再编。
- **编译报找不到 `esp_lcd` / `i2c_master`**：确认 `main/CMakeLists.txt` 的 `REQUIRES` 已含
  `esp_lcd esp_driver_i2c`；改过 CMakeLists 后先 `idf.py reconfigure` 再 `build`。
- **I2C 地址扫描**：若以上都不行，可用 `i2cdetect` 类小程序确认屏的真实地址。

---

## L5 · 按键 + 状态机骨架（本关交付）

在 L4 基础上引入一个**状态机**作为"中枢"，并用**三个实体按键**分别触发不同功能。这是把零散的
LED / OLED / 喇叭真正串成"机器人"的关键一步：以后无论谁（按键、WiFi 事件、定时器）想改变状态，
只喊一声 `bot_set_state(下一个状态)`，屏幕 / 灯 / 喇叭就自动跟上，不用到处散落控制代码。

本关定义四个状态：`IDLE`（空闲）→ `LISTENING`（聆听）→ `THINKING`（思考）→ `SPEAKING`（说话）。
三个按键各司其职（接线见下）：
- **按键1（GPIO0 唤醒）**：把状态切到 `LISTENING`，开始一次聆听/对话（模拟语音唤醒词）
- **按键2（GPIO39 音量-）**：喇叭音量 -10（到底 0），屏幕显示 `VOL xx%`
- **按键3（GPIO40 音量+）**：喇叭音量 +10（封顶 100），屏幕显示 `VOL xx%`

### 接线表（按键 → ESP32-S3）

| 按键 | 功能 | ESP32-S3 | 说明 |
|------|------|----------|------|
| 按键1 | 语音唤醒 | GPIO0  | 一端接 GPIO0，另一端接 GND（内部已开上拉；按下=下降沿） |
| 按键2 | 音量 - | GPIO39 | 一端接 GPIO39，另一端接 GND |
| 按键3 | 音量 + | GPIO40 | 一端接 GPIO40，另一端接 GND |

> ⚠️ **GPIO0 是 ESP32-S3 的 strapping 脚**（上电/复位瞬间决定启动模式）。用作"唤醒键"完全没问题，
> 但**不要在板子复位或上电的瞬间按住它**，否则芯片会进下载模式而不是正常启动。
> ⚠️ **GPIO39/40 默认是 JTAG 脚**（TMS/TDI）。当普通输入用没问题，代价是以后没法把外部 JTAG
> 调试器插到这几个脚上（USB 串口烧录不受影响）。
> 按键脚在 `button.h` 顶部的宏 `BTN_WAKE_GPIO` / `BTN_VOL_DOWN_GPIO` / `BTN_VOL_UP_GPIO` 里。
> 一个两脚轻触开关即可；若想"按下弹起都触发"，把 `GPIO_INTR_NEGEDGE` 改成 `GPIO_INTR_ANYEDGE`。

### 这一关新增 / 变更了什么

- **新增 `state_machine.h` / `state_machine.c`**：状态枚举 `bot_state_t` + `bot_set_state()`。
  切状态时它会**自动**做三件事：刷 OLED 显示 `STATE: xxx`、把板载 RGB 灯设成对应颜色
  （蓝=聆听 / 紫=思考 / 绿=说话 / 灭=空闲）、进入 `SPEAKING` 时额外播一声 1kHz 测试音（模拟开口）。
- **新增 `button.h` / `button.c`**：支持**三个按键**，用 GPIO 中断 + FreeRTOS 队列实现。
  三个按键共用同一套 ISR/队列/任务，ISR 用参数把"哪个 gpio 被按了"带进来，入队后在
  `button_task` 里消抖、再映射成 `button_action_t`（唤醒/音量-/音量+）交给上层回调。
  中断里只往队列丢信号，消抖和"调回调"在 `button_task` 里做（中断必须快进快出）。
- **改 `led.c` / `led.h`**：把灯带初始化从"心跳任务"抽成 `led_init()`，新增 `led_set_color(r,g,b)`
  和 `led_off()`。L5 之后灯的颜色**由状态机决定**，不再有心跳闪烁。
- **改 `oled.c` / `oled.h`**：移除 L4 的 `oled_task` 自动轮播（演示交给状态机事件驱动）。
- **改 `audio_out.h` / `audio_out.c`**：新增音量控制
  （`audio_out_set_volume` / `get_volume` / `volume_up` / `volume_down`，音量 0~100）。
  `play_tone` 生成正弦波时按当前音量缩放振幅——这就是"调音量"的本质。
- **改 `main.c`**：初始化所有模块 → `bot_init()` → 注册按键回调 → 创建 `mic_task` + `button_task`。
  按键回调 `on_button_pressed(action)` 按动作分派：唤醒→`LISTENING`，音量±→调音量并在 OLED 显示 `VOL xx%`。
- **改 `CMakeLists.txt`**：`SRCS` 加了 `button.c` `state_machine.c`。

### 编译 / 烧录 / 监视

```bash
idf.py build
idf.py -p COM3 flash monitor     # COM3 换成你的端口
```

> 改过 CMakeLists 后若 IDE 没自动重配，先 `idf.py reconfigure` 再 `build`。

### ✅ 验收标准（通关条件）

1. 启动日志应看到（顺序可能略有不同）：
   ```
   I (xxx) led: 板载 RGB LED 已就绪 (GPIO48)
   I (xxx) oled: OLED 已初始化 128x64 @ I2C 0x3C (SDA=GPIO41, SCL=GPIO42)
   I (xxx) state: 状态机已初始化，当前 IDLE
   I (xxx) button: 按键已就绪 (唤醒=GPIO0, 音量-=GPIO39, 音量+=GPIO40, 下降沿触发)
   I (xxx) main: 系统就绪
   ```
2. 屏幕先静态显示 `STATE: IDLE`。
3. **按按键1（唤醒 GPIO0）**：屏幕变 `STATE: LISTENING`、板载 RGB 灯变蓝。
   反复按唤醒键，若已在 LISTENING 则状态机忽略同状态（不会重复处理）。
4. **按按键2（音量- GPIO39）/ 按键3（音量+ GPIO40）**：
   - 日志出现 `audio_out: 音量设置为 xx%`
   - 屏幕显示 `VOL: xx%`（实时反映新音量）
   - 进入 SPEAKING 时喇叭"嘀"一声（1000Hz，约 800ms），且**响度随音量变化**（音量 0 = 静音）
5. 麦克风仍在后台打印 RMS（L2 功能没丢），但 LED 不再自动闪、喇叭不再每 3 秒自动"嘀"
   （出声改由状态机在 SPEAKING 时驱动，更符合"机器人说话才出声"的语义）。

### 🐞 常见坑

- **按了没反应**：① 确认按键另一端真的接到 GND、且对应脚没被别的设备占用；
  ② 用万用表/逻辑分析仪看按下时对应 GPIO 是否从高变低；③ 看日志有没有 `button: 按键 GPIOx -> 动作 y`，
  没出现就是中断没进来（检查 `gpio_install_isr_service` 是否被别处重复安装导致失败）。
- **音量键没反应 / 音量卡住**：确认 `main.c` 里 `on_button_pressed` 的 `case BTN_VOL_DOWN/UP`
  分支接到了 `audio_out_volume_down/up`；音量到 0 或 100 时会被自动夹住（属正常）。
- **板子一上电就进下载模式**：多半是 GPIO0 的唤醒键在复位瞬间被按住（stapping 脚特性）。
  松开按键、重新上电即可；平时使用无影响。
- **一次按下被当成多次（连跳）**：`button.c` 里已有 50ms 软件消抖；若仍连跳，把
  `BUTTON_DEBOUNCE_MS` 调大到 80~100。
- **屏幕/灯只变一次就不再变**：检查 `bot_set_state` 里是否加了"同状态不重复处理"的
  `if (new_state == g_state) return;`——连按同一状态本就会被忽略，属正常。
- **喇叭在 SPEAKING 没"嘀"**：确认 `audio_out_init()` 已先被调用；进入 SPEAKING 后日志应出现
  `audio_out: 播放测试音 1000 Hz, 800 ms 完成`。

---

过完 L5，在对话里回我「**L5 过了**」或贴出监视器日志，我接着给 **L6 WiFi + NVS + SoftAP 配网** 的代码与接线（那时会真正连上你的路由器）。
