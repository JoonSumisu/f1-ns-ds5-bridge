# 预编译固件

| 文件 | 板 | 版本 | 烧录(esptool) |
|---|---|---|---|
| `gate2_ds5_v5.4b.bin` | ESP32-S3 接收板 | v5.4b-final ⭐ | `write_flash 0x10000` |
| `tx_bluepad32_v5.4.bin` | ESP32 发射板 | v5.4 | `write_flash 0x10000`(板需已有 Bluepad32 bootloader/分区表;全新板请源码编译) |

完整命令与验证步骤见根目录 `AGENTS.md`。

## 版本配对(重要)

- TX v5.4 起 UART 波特率 **921600**,必须配 RX ≥ v5.4
- 旧组合(TX@460800 + RX v5.3)彼此兼容,但不与本目录文件互换——混刷的症状是 DS5 报文全零

## 版本史

- **v5.4b-final**:当前最终版。时间戳为墙钟差值×3(v5.4 的"等间隔化"实验在 Steam 融合器上翻车,已回退,教训见 research-notes.md 第 1 节)
- v5.4:UART 提速 921600、通知唤醒、看门狗 panic、USB 停滞自愈、电池上报修正
- v5.3:时间戳单位修正(25µs → 0.33µs)、按键映射定案

校验:`SHA256SUMS.txt`
