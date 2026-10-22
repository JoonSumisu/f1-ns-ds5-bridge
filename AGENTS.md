# AGENTS.md — AI 刷机操作手册

本文件写给"正在帮人类把这套固件刷进硬件的 AI 助手"。目标:一次会话内完成两块板的刷入与验证,不踩已知的坑。人类只需要按你的指示按按键。

## 系统总览(30 秒)

两块板、两条固件,**方向不要搞反**:

| 板 | 芯片 | 固件 | 出口 |
|---|---|---|---|
| TX 发射板 | ESP32(经典蓝牙,如 DevKit/CP2102) | Bluepad32 + 本仓库 `firmware/tx-bluepad32/my_platform.c` | UART1 GPIO25 → RX 板 GPIO5 |
| RX 接收板 | ESP32-S3(必须有原生 USB 口) | 本仓库 `firmware/s3-ds5`(ESP-IDF 工程) | 原生 USB → PC,枚举为 DualSense |

用户只想尽快用起来:直接刷 `bin/` 里的现成固件,跳过编译。

## 路径 A:直接刷 bin(推荐)

前置:python3 + esptool(`pip install esptool`)。

### RX 板(ESP32-S3)

1. 找端口:
   - macOS:`ls /dev/cu.usbmodem*`(下载模式下出现)
   - Linux:`ls /dev/ttyACM*`
   - Windows:设备管理器 → 端口 (COMx)
2. 进下载模式:**按住 BOOT,点按 RST,保持 1 秒,松开 BOOT**,然后插 USB。
   ⚠️ S3 上 esptool 的 RTS 自动复位经常无效,人工按键最稳;数据线必须是数据线(充电线没有数据芯,是最常见的"连不上"原因)。
3. 烧录:
   ```bash
   python3 -m esptool --port <PORT> -b 460800 write_flash 0x10000 bin/gate2_ds5_v5.4b.bin
   ```
   只刷 app(@0x10000),依赖板子出厂 bootloader/分区表(绝大多数 S3 开发板默认布局满足)。若启动时串口反复报 partition 错误,需要补刷 bootloader@0x0 与分区表@0x8000——先和用户确认板子,再动手。
4. RST 复位。主机应枚举出 Sony DualSense(VID 054C / PID 0CE6)。

### TX 板(ESP32 经典)

1. CP2102 USB 口插入,macOS 端口形如 `/dev/cu.usbserial*`。
2. ESP32 经典芯片 esptool 自动复位可用:
   ```bash
   python3 -m esptool --chip esp32 --port <PORT> -b 460800 write_flash 0x10000 bin/tx_bluepad32_v5.4.bin
   ```
   此命令适用于**已跑过 Bluepad32 的板子**(bootloader/分区表已在)。全新板子走路径 C 全量刷写。

### 版本配对(重要)

TX v5.4 起 UART 波特率 921600,**必须配 RX ≥ v5.4**;旧 TX(460800)配旧 RX v5.3。混刷症状:RX 收不到任何帧,DS5 报文全零。

## 验证

1. **枚举**:
   - macOS:`system_profiler SPUSBDataType | grep -A5 -i sony`
   - Linux:`lsusb | grep 054c:0ce6`
   - Windows:设备管理器人眼确认
2. **报文流**:`pip install hidapi` 后运行 `python3 tools/ds5_probe.py`(3 秒采样):
   - 正常:`fps≈170`、`seq bad=0`、静置 `gyro_max` < 50 counts、摆动手柄数千
   - 摇动时 gyro_max 不动 → UART 链路断(查接线/波特率配对)
   - seq bad 多 → UART 误码(查共地/线长)
3. **振动链路**:进游戏开一枪 → RX 板串口日志出现 `振动转发` → 手柄震动。注意:振动只在游戏真实反馈时出现,桌面/Steam 界面不震是**正确行为**。

## 路径 B:编译 RX 固件(ESP-IDF v5.5.x)

```bash
cd firmware/s3-ds5
idf.py set-target esp32s3   # 仅首次
idf.py build flash monitor
```

TinyUSB 组件按 `dependencies.lock` 自动拉取;sdkconfig 已含 WDT panic、USB 配置等全部定案项,不要重新生成。

## 路径 C:编译 TX 固件(基于上游 Bluepad32,便于跟进上游)

```bash
git clone https://github.com/ricardoquesada/bluepad32
cp firmware/tx-bluepad32/my_platform.c bluepad32/examples/esp32/main/my_platform.c
cd bluepad32/examples/esp32
idf.py set-target esp32 && idf.py build flash monitor
```

Bluepad32 的 IDF 依赖见其官方 README。

## 接线(详见 docs/wiring.md)

TX GPIO25 → RX GPIO5;TX GPIO26 ← RX GPIO6;GND↔GND;供电桥可选。**刷机前先把两板间的杜邦线拔掉**,刷完再接。

## 供电安全铁律(违反会烧板,已付出一块 S3 的代价)

这套装置的任何 USB 线,**同一时刻只能连同一台电脑**(或全部用充电头)。绝对禁止两台电脑同时挂在线缆网里——哪怕其中一台只是经杜邦线共地。两台电脑的地电位差会在热插拔瞬间经共地线泄放,浪涌足以击穿 USB 口 VBUS 上的保护二极管。刷机要换电脑:先把另一台侧所有线拔干净。

## 行为红线(改代码前必读,均为实测教训)

1. **振动只允许来自游戏 0x02 输出报告的真实反馈**。禁止人工合成震动、待机脉冲、扳机反馈模拟——全部试过、全部被否。
2. **禁止加平滑/滤波**。延迟红线;手感基准 = 同手柄 NS 档直连 PC。
3. **陀螺仪刻度 mdps/893 是定案值**,不要"修正"成名义 16.384 c/dps(会偏 14.7%,实测手感不对;背景见 docs/research-notes.md)。
4. **传感器时间戳 = 墙钟差值×3**(0.33µs 单位)。任何时间戳语义改动,必须先用 `tools/ds5_probe.py` 验证 dt 直方图均匀(~16.7k counts 一档)再进游戏——v5.4 的"等间隔化"改动没走这步,造成游戏内陀螺仪失控,已回退。
5. Bluepad32 侧不能从非 btstack 任务直接调其 API(`execute_on_main_thread` 会触发信号量断言崩溃),跨任务用标志位(见 my_platform.c 注释)。

## 常见坑速查

| 症状 | 原因/处理 |
|---|---|
| esptool 连不上 S3 | 手动 BOOT+RST;换条真数据线;换 USB 口 |
| 刷完无设备 | 串口 monitor 看分区报错→补 bootloader/分区表;换一根线(供电不足也会) |
| 游戏里陀螺仪狂转/漂 | 刻度或时间戳被改动,对照红线 3/4;用 probe 验证 |
| 手柄一直嗡震 | 0x02 使能位门控被破坏:Steam 音量/灯光帧的振动字节非零被误当振动 |
| 报文全零 | TX/RX 波特率不配对(921600/460800);TX GPIO25↔RX GPIO5 接反 |
| 手柄连不上 TX 板 | 手柄切 NS 档;已配对过的话 TX 板重启后手柄会自动回连,等几秒 |
| USB 口突然彻底无反应 | 多半 VBUS 保护二极管(SS34/SS54)被浪涌击穿,换板或参考 docs/wiring.md 修复节 |
