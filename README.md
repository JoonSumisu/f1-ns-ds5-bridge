# f1-ds5-bridge

把 NS 档(经典蓝牙)游戏手柄的陀螺仪转译成 PC 上的 **DualSense(PS5 手柄)**——让 COD/MW4 等原生支持 DS5 体感瞄准的游戏,用你顺手的手柄打 gyro aiming。

实测载体为 MACHENIKE F1(NS 档);理论上任何 Bluepad32 支持的 Switch 档经典蓝牙手柄都适用。

## 架构

```
F1 手柄 ═经典蓝牙(NS档)═> ESP32 发射板(Bluepad32)
                              │ UART 921600, 44字节帧
                              ▼
                          ESP32-S3 接收板(TinyUSB) ═USB═> PC(识别为 DualSense)
```

- **发射板(TX)**:任意 ESP32 经典蓝牙开发板(原版 DevKit / CP2102 均可),跑 Bluepad32,负责配对手柄,把 IMU/按键/电池打包成 44 字节 UART 帧
- **接收板(RX)**:ESP32-S3(需带原生 USB 口),TinyUSB 实现 DS5 64 字节输入报告与 0x02 输出报告(振动转发),USB VID/PID = 054C:0CE6

## 特性

- 陀螺仪/加速度计三采样突发转发,DS5 报文率实测 ~175Hz(上限 ~180Hz,受手柄 NS 蓝牙协议限制)
- 陀螺仪刻度对齐 Switch 直连基准(mdps/893,与 SDL Switch 手感一致,见协议文档)
- DS5 传感器时间戳语义(0.33µs 单位)——姿态融合类功能正常工作的关键
- 电池上报(Bluepad32 档位 → DS5 0-10 编码)
- 振动转发:仅游戏真实反馈,带使能位门控与地板值过滤(Steam 常驻的音量/灯光帧不会误震)
- 按键重映射:Switch 逻辑键 → Xbox 丝印语义(A/B/X/Y 对调);SELECT 短按=触摸板点击、长按=CREATE
- L2/R2 数字扳机自动合成满行程(NS 协议扳机只有数字量)
- 看门狗 + USB 停滞自愈(8 秒端点不就绪自动重启)
- 固件样貌仿真(fw 0x0630),避免被 Steam 判为未知设备

## 快速开始

1. **[docs/wiring.md](docs/wiring.md)** —— 接线与供电(先读"安全铁律",这是烧掉一块 S3 换来的)
2. **[AGENTS.md](AGENTS.md)** —— 刷机手册(为 AI 辅助刷机场景写的,人看也一样清楚)
3. 不想编译就直接刷现成固件:**[bin/](bin/)**

## 文档索引

| 文件 | 内容 |
|---|---|
| [AGENTS.md](AGENTS.md) | AI 刷机操作手册:工具链、命令、验证、坑 |
| [docs/protocol.md](docs/protocol.md) | UART 帧协议 / DS5 报文布局 / 刻度换算定案 |
| [docs/wiring.md](docs/wiring.md) | 接线表、供电方案、安全规则 |
| [docs/research-notes.md](docs/research-notes.md) | 调研与发现记录:时间戳单位、轴向验证、刻度定案、常震根因、烧板分析等 |
| `firmware/s3-ds5/` | 接收板完整 ESP-IDF 工程 |
| `firmware/tx-bluepad32/` | 发射板定制文件(替换进上游 Bluepad32) |
| `tools/ds5_probe.py` | 上电后验证 DS5 报文流的小工具 |

## 已知限制

- IMU 上限 ~180Hz:手柄的 NS 蓝牙协议就是 60Hz×3 采样,固件层改不动
- 无触摸板数据(F1 没有触摸板,报告恒"未触摸")
- 自适应扳机效果不转发(F1 无该硬件),光条/玩家灯不实现(直接忽略)
- MW4 的"武器控制(Weapon Control)"体感模式在部分平台存在反转 bug(真 Switch 2 手柄同样中招),与本转译无关

## 致谢

- [Bluepad32](https://github.com/ricardoquesada/bluepad32) —— 发射板蓝牙手柄主机栈
- Espressif TinyUSB、[SDL hidapi](https://github.com/libsdl-org/SDL)、Linux `hid-playstation` —— DS5 报文语义与刻度的参照实现

版本:v5.4b-final(2026-10)
