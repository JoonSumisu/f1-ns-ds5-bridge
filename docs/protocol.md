# 协议与报文

## 1. UART 链路(921600 8N1)

TX 板 GPIO25 → RX 板 GPIO5(数据);RX 板 GPIO4/GPIO6 → TX 板 GPIO26(振动回传,两脚同输出,接任一);两板必须共地。

### 1.1 数据帧(TX → RX,44 字节,~40Hz 突发)

帧内 accel/gyro 为本次回调的 3 组采样中的最新一组;RX 侧为每组采样各发一帧 DS5 报文(突发三连发),实测 DS5 报文率 ~175Hz。

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0..1 | u8×2 | 魔数 0xA5 0x5A(便于对端失步后重新同步) |
| 2 | u8 | 帧序号(每帧 +1) |
| 3 | u8 | 帧长 44 |
| 4..7 | u32 LE | Bluepad32 buttons 位图 |
| 8..9 | u16 LE | brake(L2)0..1023 |
| 10..11 | u16 LE | throttle(R2)0..1023 |
| 12..19 | i16×4 LE | lx ly rx ry(-512..511) |
| 20..25 | i16×3 LE | accel(Switch 刻度,4096/g) |
| 26..37 | i32×3 LE | gyro(mdps) |
| 38 | u8 | connected(1/0) |
| 39 | u8 | battery(TX 侧语义:0=空,254=满,255=无上报) |
| 40 | u8 | dpad 位(UP=bit0 DOWN=1 RIGHT=2 LEFT=3) |
| 41 | u8 | misc_buttons(SYSTEM=0 SELECT=1 START=2 CAPTURE=3) |
| 42 | u8 | 保留 0 |
| 43 | u8 | CRC8(poly 0x07,init 0,覆盖 [0..42]) |

### 1.2 振动回传(RX → TX,行协议)

```
R<weak>,<strong>\n    // 0..255
```

RX 侧仅当游戏 0x02 报告的使能位置位且幅值 ≥70(RUMBLE_FLOOR)时发送;激活期间每 200ms 重发维持;**停止命令不发送**(TX 侧单次播放 250ms 自停)。原因见 research-notes.md 第 5 节。

## 2. DS5 输入报告 0x01(RX → PC,64 字节)

| 偏移 | 字段 |
|---|---|
| 0 | 0x01 |
| 1..4 | lx ly rx ry(0..255,中位 128) |
| 5..6 | l2 r2(0..255) |
| 7 | seq(递增) |
| 8..11 | buttons3 组(见下) |
| 12..15 | 保留 |
| 16..21 | gyro i16×3 LE(名义 16 counts/dps;本固件填 mdps/893,见 §5) |
| 22..27 | accel i16×3 LE(~8192 counts/g;Switch 刻度×2) |
| 28..31 | 传感器时间戳 LE(**0.33µs 单位** = 报告间隔µs×3) |
| 32 | 保留 |
| 33..40 | 触点×2(每触点 4 字节,首字节 bit7=1 表示未触摸) |
| 41..52 | 保留 |
| 53..55 | status:[53] 低4位电量 0..10(×10+5=%) 高4位充电态;[54]=0x08(USB 连接) |
| 56..63 | 调试遥测:[56] 构建标记(0x54=v5.4 系)、[57] 输出包序号、[58..63] 最近 0x02 输出报告摘要(EnableBits/振动字节/扳机类型)。Windows HID 输入报告广播给所有句柄,PC 侧可并行读取;游戏不解析保留尾字节 |

按键位:`buttons[0]` 低4位 hat(0=N 顺时针 7=NW,8=松开)+ bit4 SQUARE bit5 CROSS bit6 CIRCLE bit7 TRIANGLE;`buttons[1]` bit0 L1、1 R1、2 L2、3 R2、4 CREATE、5 OPTIONS、6 L3、7 R3;`buttons[2]` bit0 PS、1 TOUCHPAD、2 MIC_MUTE。

**轴向映射(Switch → DS5)**:accel 与 gyro 同矩阵 `(-Y_sw, +Z_sw, -X_sw)`,与 SDL2 对真 DS5 的处理一致(验证链见 research-notes.md 第 2 节)。

**按键语义**:Switch 逻辑键转 Xbox 丝印(丝印 A→CROSS、B→CIRCLE、X→SQUARE、Y→TRIANGLE);SELECT 短按=TOUCHPAD 点击(释放时合成 80ms)、长按 ≥500ms=CREATE 按住;HOME→PS 直通;CAPTURE→TOUCHPAD 直通。

## 3. DS5 输出报告 0x02(PC → RX)

b[0]=0x02;b[1] EnableBits1(bit0=经典振动);b[2] EnableBits2(0x04 灯色、0x10 玩家灯);b[3] RumbleRight(高频/右手)、b[4] RumbleLeft(低频);b[11..32] 自适应扳机效果(**忽略不转发**);b[39] EnableBits3(bit2=增强振动)。

振动字节**必须由使能位门控**:Steam 常驻的音量/灯光帧这些字节非零但使能位未置,误读 = 常震。灯条/玩家灯/音频一概不转发。

## 4. Feature 报告(RX 应答)

- **0x20 固件信息**:全零会被 Steam 判未知设备(陀螺仪不校准直读,实测 16 倍过灵敏)。本固件回填 hardware/fw/update 样貌,update=0x0630(SDL 以此判 `firmware_version ≥ 0x0224` 走增强震动路径)。
- **0x09 配对信息**:MAC 94:CE:2A:11:22:33,与 USB 序列号一致。
- **0x05 校准数据**:回静置标称值(speed ±512、accel ±8192),使主机直读 IMU 不做软件校准。

## 5. 刻度与时间戳(定案值,勿改,背景见 research-notes.md)

- gyro:counts = **mdps / 893**(对齐 Switch 直连手感;名义 16.384 会低灵敏 14.7%)
- accel:Switch counts × 2(4096/g → 8192/g)
- 时间戳:单位 **0.33µs**(内核 hid-playstation:ts/3=µs),本固件 = 报告墙钟间隔µs×3

## 6. USB 身份

VID:PID = **054C:0CE6**(Sony DualSense),序列号/配对 MAC 94:CE:2A:11:22:33;HID 输入报告 0x01(64B)/ 输出报告 0x02,接口含一个中断 IN + 一个中断 OUT 端点(见 `firmware/s3-ds5/main/usb_descriptors.c`)。
