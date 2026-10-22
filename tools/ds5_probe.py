#!/usr/bin/env python3
"""DS5 报文流探针: 验证接收板固件是否正常工作.

用法: pip install hidapi && python3 ds5_probe.py
判读 (3 秒采样):
  fps≈170          报文率正常 (上限 ~180, 受手柄 NS 协议限制)
  seq bad=0        UART 链路无丢帧; bad 多 -> 查共地/线缆/波特率配对
  gyro_max<50      静置时噪声; 摆动手柄应到数千
  ts_deltas        时间戳差分布, 正常应集中在 ~16.7k (0.33us 单位) 一档;
                   出现 0/25ms 双峰 = 时间戳语义被改坏 (勿进游戏, 见 research-notes.md)
"""
import hid, time, struct

dev = None
for d in hid.enumerate(0x054C, 0x0CE6):
    dev = hid.device(); dev.open_path(d['path']); break
if not dev:
    print("NO DEVICE (未找到 054C:0CE6, 检查接收板是否枚举)")
    raise SystemExit(1)

n = 0; t_prev = None; ts_deltas = {}; gmax = 0
seq_ok = seq_bad = 0; prev_seq = None; batt = None
t0 = time.time()
while time.time() - t0 < 3:
    r = bytes(dev.read(64, 50))          # bytes() 兼容旧版 hidapi 返回 list
    if not r or len(r) < 64 or r[0] != 1:
        continue
    n += 1
    gx, gy, gz = struct.unpack_from('<3h', r, 16)
    gmax = max(gmax, abs(gx), abs(gy), abs(gz))
    ts = struct.unpack_from('<I', r, 28)[0]
    if t_prev is not None:
        d = (ts - t_prev) & 0xFFFFFFFF
        ts_deltas[d // 1000 * 1000] = ts_deltas.get(d // 1000 * 1000, 0) + 1
    t_prev = ts
    s = r[7]
    if prev_seq is not None:
        if (prev_seq + 1) & 0xFF == s: seq_ok += 1
        else: seq_bad += 1
    prev_seq = s
    batt = r[53]

print(f"frames={n} fps={n // 3}")
print(f"gyro_max_counts={gmax} (~{gmax / 16:.0f} dps @16c/dps 名义)")
print(f"ts_deltas(0.33us单位, 千位桶): {sorted(ts_deltas.items(), key=lambda x: -x[1])[:6]}")
print(f"seq ok={seq_ok} bad={seq_bad}")
print(f"batt_status_byte=0x{batt:02x} level={batt & 0xF} ({(batt & 0xF) * 10 + 5}%)")
