# TX 发射板固件(Bluepad32 定制)

本目录只有一个文件 `my_platform.c`,**替换**上游 Bluepad32 ESP32 例程中的同名文件:

```bash
git clone https://github.com/ricardoquesada/bluepad32
cp my_platform.c bluepad32/examples/esp32/main/my_platform.c
cd bluepad32/examples/esp32
idf.py set-target esp32 && idf.py build flash monitor
```

Bluepad32 自身的依赖安装见其官方 README。

## 相对上游的改动

- UART1 @921600:GPIO25 输出 44 字节 IMU/按键/电池帧(布局见 `docs/protocol.md`)
- GPIO26 接收振动命令 `R<w>,<s>\n`,经 btstack 回调转发到手柄。跨任务用标志位而不是直接调 API(非 btstack 任务调 Bluepad32 API 会触发信号量断言崩溃)
- 三采样突发转发:每次控制器数据回调把 3 组 IMU 采样逐帧发出
- `R0,0` 特判为停止,走 `play_dual_rumble(0,0,0,0)` 停止路径(Bluepad32 的 Switch rumble 编码幅值写死 500,任何命令都会触发一次满幅震动,见 research-notes.md 第 5 节)
- 电池周期刷新:约每 60 秒发一次无振动子命令,借 0x21 ACK 更新电量
