**[简体中文](README.md)** | English

# f1-ds5-bridge

Translates the gyroscope of a Switch-mode (Classic Bluetooth) gamepad into a **DualSense (PS5 controller)** on PC — so games with native DS5 motion aiming like COD/MW4 can be played with the controller you actually like, using gyro aiming.

Built and tested with the MACHENIKE F1 (NS mode); in principle any Switch-mode Classic Bluetooth gamepad supported by Bluepad32 should work.

## Architecture

```
F1 gamepad ═Classic BT (NS mode)═> ESP32 TX board (Bluepad32)
                                        │ UART 921600, 44-byte frames
                                        ▼
                                   ESP32-S3 RX board (TinyUSB) ═USB═> PC (enumerates as DualSense)
```

- **TX board**: any ESP32 Classic Bluetooth dev board (original DevKit / CP2102 both fine), runs Bluepad32, pairs with the gamepad and packs IMU/buttons/battery into 44-byte UART frames
- **RX board**: ESP32-S3 (must have a native USB port), implements the DS5 64-byte input report and 0x02 output report (rumble forwarding) via TinyUSB; USB VID/PID = 054C:0CE6

## Features

- Gyro/accel burst forwarding, measured ~175Hz DS5 report rate (ceiling ~180Hz, limited by the gamepad's NS Bluetooth protocol)
- Gyro scale aligned to the Switch-direct baseline (mdps/893, matches SDL Switch feel — see protocol docs)
- Correct DS5 sensor timestamp semantics (0.33µs unit) — critical for any attitude-fusion consumer
- Battery reporting (Bluepad32 tiers → DS5 0-10 encoding)
- Rumble forwarding: only real game feedback, gated by enable bits with a floor filter (Steam's persistent volume/lightbar frames never cause phantom rumble)
- Button remapping: Switch logical layout → Xbox silkscreen semantics (A/B/X/Y swapped); SELECT short-press = touchpad click, long-press = CREATE
- L2/R2 digital triggers synthesized to full travel (NS protocol triggers are digital-only)
- Watchdog + USB stall self-healing (auto-restart if the endpoint isn't ready for 8s)
- Firmware identity spoofing (fw 0x0630) to avoid Steam's "unknown device" path

## Quick Start

1. **[docs/wiring.md](docs/wiring.md)** — wiring & power (read the "safety rules" first; they were paid for with one burned S3) *(Chinese)*
2. **[AGENTS.md](AGENTS.md)** — flashing handbook (written for AI-assisted flashing; works for humans too) *(Chinese)*
3. Don't want to compile? Flash the prebuilt binaries: **[bin/](bin/)**

## Documentation Index

| File | Content |
|---|---|
| [AGENTS.md](AGENTS.md) | AI flashing handbook: toolchain, commands, verification, pitfalls |
| [docs/protocol.md](docs/protocol.md) | UART frame protocol / DS5 report layouts / scale conversions (final) |
| [docs/wiring.md](docs/wiring.md) | Wiring table, power schemes, safety rules |
| [docs/research-notes.md](docs/research-notes.md) | Research log: timestamp unit, axis verification, scale decision, rumble root causes, the burned-port analysis |
| `firmware/s3-ds5/` | RX board, complete ESP-IDF project |
| `firmware/tx-bluepad32/` | TX board customization (drop-in file for upstream Bluepad32) |
| `tools/ds5_probe.py` | Small tool to verify the DS5 report stream after power-on |

*(Docs are currently Chinese-only; the code and command blocks are self-explanatory.)*

## Known Limitations

- IMU ceiling ~180Hz: the gamepad's NS Bluetooth protocol carries 60Hz×3 samples; firmware cannot raise it
- No touchpad data (the F1 has no touchpad; reports always say "not touching")
- Adaptive trigger effects are not forwarded (F1 has no such hardware); lightbar/player-LED ignored
- MW4's "Weapon Control" motion mode has an inversion bug on some platforms (real Switch 2 controllers are affected too); unrelated to this bridge

## Contributors

- **[JoonSumisu](https://github.com/JoonSumisu)** — hardware, feel calibration, in-game testing, and one burned S3 as tuition :)
- **GLM-5.3** (ZCode AI assistant) — firmware development, DS5 protocol research, end-to-end debugging (v5.0 → v5.4b), and these docs

## Acknowledgements

- [Bluepad32](https://github.com/ricardoquesada/bluepad32) — Bluetooth gamepad host stack (TX board)
- Espressif TinyUSB, [SDL hidapi](https://github.com/libsdl-org/SDL), Linux `hid-playstation` — reference implementations for DS5 report semantics and scales

Version: v5.4b-final (2026-10)

## Buy Me a Coffee

If this project helps you, a small tip is welcome — it fuels the AI tokens that wrote the firmware ☕

<p align="center"><img src="assets/wechat_pay.jpg" width="220" alt="WeChat Pay QR"></p>
