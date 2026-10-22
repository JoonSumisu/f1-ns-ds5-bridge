// v3: 三采样突发转发(~120Hz) + 电池周期刷新 + 振动回传 (S3 UART RX -> F1)
// F1(NS档, 经典蓝牙) --Bluepad32--> 本板 --UART1 GPIO25(TX) 921600--> S3
// 振动: S3 GPIO4(TX) -> 本板 GPIO26(RX), 行协议 "R<weak>,<strong>\n" (0-255)
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <uni.h>

#define TX_PIN    25
#define RX_PIN    26
#define UART_BAUD 921600
#define FRAME_LEN 44
#define BAT_REFRESH_CALLS 2400  // ~60s @40Hz: 发无振动子命令触发 0x21 ACK 刷新电池

// IMU 统计与节流
static int s_data_calls = 0;
static int32_t s_gmax[3] = {0};
static uint8_t s_seq = 0;

static uni_hid_device_t* s_dev = NULL;  // 振动目标, 仅 btstack 上下文读写

// ---- 振动: UART 任务只置标志, 由 on_controller_data (btstack 上下文, 120Hz) 代发 ----
// 注意: 不能从其他任务直接调 btstack API (execute_on_main_thread 会触发
// xQueueSemaphoreTake 断言崩溃), 改为标志位 + 回调内消化, 延迟最多 ~8ms.
static volatile uint8_t s_rumble_weak = 0, s_rumble_strong = 0;
static volatile bool s_rumble_pending = false;

// CRC8 (poly 0x07, init 0)
static uint8_t crc8(const uint8_t* p, int len) {
    uint8_t c = 0;
    for (int i = 0; i < len; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

// 发一帧; accel/gyro 由调用者指定 (三采样转发用), 其余字段取 gp
static void uart_send_frame_ex(const uni_gamepad_t* gp, const int32_t* accel,
                               const int32_t* gyro, uint8_t battery, bool connected) {
    uint8_t f[FRAME_LEN];
    memset(f, 0, sizeof(f));
    f[0] = 0xA5; f[1] = 0x5A;                 // 双魔数便于对端重新同步
    f[2] = s_seq++;
    f[3] = FRAME_LEN;
    // [4..7] buttons u32
    f[4] = gp->buttons & 0xFF;
    f[5] = (gp->buttons >> 8) & 0xFF;
    f[6] = (gp->buttons >> 16) & 0xFF;
    f[7] = (gp->buttons >> 24) & 0xFF;
    // [8..9] brake(L2) [10..11] throttle(R2)  (0..1023)
    f[8] = gp->brake & 0xFF;          f[9] = (gp->brake >> 8) & 0xFF;
    f[10] = gp->throttle & 0xFF;      f[11] = (gp->throttle >> 8) & 0xFF;
    // [12..19] lx ly rx ry (i16, -512..511)
    for (int i = 0; i < 4; i++) {
        int16_t v = (int16_t)(i == 0 ? gp->axis_x : i == 1 ? gp->axis_y :
                              i == 2 ? gp->axis_rx : gp->axis_ry);
        f[12 + i * 2] = v & 0xFF;
        f[13 + i * 2] = (v >> 8) & 0xFF;
    }
    // [20..25] accel i16 x3 (Switch counts, 4096/g)
    for (int i = 0; i < 3; i++) {
        int32_t v = accel[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        int16_t s = (int16_t)v;
        f[20 + i * 2] = s & 0xFF;
        f[21 + i * 2] = (s >> 8) & 0xFF;
    }
    // [26..37] gyro i32 x3 (mdps)
    for (int i = 0; i < 3; i++) {
        int32_t v = gyro[i];
        f[26 + i * 4] = v & 0xFF;
        f[27 + i * 4] = (v >> 8) & 0xFF;
        f[28 + i * 4] = (v >> 16) & 0xFF;
        f[29 + i * 4] = (v >> 24) & 0xFF;
    }
    f[38] = connected ? 1 : 0;
    f[39] = battery;  // 0=空, 254=满, 255=无电池上报
    f[40] = gp->dpad & 0x0F;            // DPAD_UP=bit0 DOWN=1 RIGHT=2 LEFT=3
    f[41] = gp->misc_buttons & 0x0F;    // SYSTEM=0 SELECT=1 START=2 CAPTURE=3
    f[43] = crc8(f, FRAME_LEN - 1);
    uart_write_bytes(UART_NUM_1, (const char*)f, FRAME_LEN);
}

static void uart_send_frame(const uni_gamepad_t* gp, uint8_t battery, bool connected) {
    uart_send_frame_ex(gp, gp->accel, gp->gyro, battery, connected);
}

// 手动解析 "R<weak>,<strong>" — 不用 sscanf (newlib 深栈会溢出 2KB 任务栈, 踩坏 UART 驱动)
static int parse_rumble_line(const char* s, int* w, int* g) {
    int v[2] = {0, 0};
    int idx = 0;
    if (s[0] != 'R')
        return -1;
    for (const char* p = s + 1; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            v[idx] = v[idx] * 10 + (*p - '0');
            if (v[idx] > 300)
                v[idx] = 300;
        } else if (*p == ',' && idx == 0) {
            idx = 1;
        } else {
            return -1;
        }
    }
    if (idx != 1)
        return -1;
    *w = v[0];
    *g = v[1];
    return 0;
}

// S3 -> 本板 振动命令解析 (行缓冲)
static void rx_task(void* arg) {
    ARG_UNUSED(arg);
    char line[32];
    int li = 0;
    uint8_t buf[64];
    while (1) {
        int n = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (li > 0) {
                    line[li] = 0;
                    li = 0;
                    logi("gate2-tx: RX line: '%s'\n", line);
                    int w, s;
                    if (parse_rumble_line(line, &w, &s) == 0) {
                        if (w > 255) w = 255;
                        if (s > 255) s = 255;
                        s_rumble_weak = (uint8_t)w;
                        s_rumble_strong = (uint8_t)s;
                        s_rumble_pending = true;
                    }
                }
            } else if (li < (int)sizeof(line) - 1) {
                line[li++] = c;
            }
        }
    }
}

//
// Platform Overrides
//
static void my_platform_init(int argc, const char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    logi("gate2-tx v3: init()\n");

    const uart_config_t uc = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(UART_NUM_1, &uc);
    uart_set_pin(UART_NUM_1, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_1, 512, 1024, 0, NULL, 0);
    xTaskCreate(rx_task, "uart_rx", 4096, NULL, 5, NULL);
    logi("gate2-tx v3: UART1 TX=GPIO%d RX=GPIO%d @%d, 三采样 120Hz + 振动回传\n",
         TX_PIN, RX_PIN, UART_BAUD);
}

static void my_platform_on_init_complete(void) {
    logi("gate2-tx v3: 就绪, 开始扫描 - F1 拨 NS 档长按 HOME 配对 (配对记忆保留, 重启后直连)\n");
    uni_bt_enable_new_connections_unsafe(true);
}

static uni_error_t my_platform_on_device_discovered(bd_addr_t addr, const char* name, uint16_t cod, uint8_t rssi) {
    logi("gate2-tx: [DISCOVERED] name='%s' cod=0x%06x rssi=%d\n",
         (name && name[0]) ? name : "(null)", cod, rssi);
    return UNI_ERROR_SUCCESS;
}

static void my_platform_on_device_connected(uni_hid_device_t* d) {
    logi("gate2-tx: [CONNECTED]\n");
}

static void my_platform_on_device_disconnected(uni_hid_device_t* d) {
    logi("gate2-tx: [DISCONNECTED]\n");
    // 通知 S3: 发一帧 connected=0
    uni_gamepad_t zero;
    memset(&zero, 0, sizeof(zero));
    uart_send_frame(&zero, 255, false);
    s_dev = NULL;
}

static uni_error_t my_platform_on_device_ready(uni_hid_device_t* d) {
    logi("gate2-tx: [READY] name='%s' VID=0x%04x PID=0x%04x\n",
         d->name, uni_hid_device_get_vendor_id(d), uni_hid_device_get_product_id(d));
    s_data_calls = 0;
    memset(s_gmax, 0, sizeof(s_gmax));
    s_dev = d;
    return UNI_ERROR_SUCCESS;
}

static void my_platform_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;
    uni_gamepad_t* gp = &ctl->gamepad;

    // 三采样转发: 旧 -> 新, USB 侧有效 ~120Hz (Bluepad32 parser 扩展字段)
    uart_send_frame_ex(gp, gp->f1_accel0, gp->f1_gyro0, ctl->battery, true);
    uart_send_frame_ex(gp, gp->f1_accel1, gp->f1_gyro1, ctl->battery, true);
    uart_send_frame_ex(gp, gp->accel, gp->gyro, ctl->battery, true);

    // 振动请求消化 (btstack 上下文, 安全调用 parser)
    if (s_rumble_pending && d) {
        s_rumble_pending = false;
        if (d->report_parser.play_dual_rumble) {
            // R0,0 = 停止: Bluepad32 的 switch rumble 编码幅值写死 500 (parser
            // uni_hid_parser_switch.c switch_play_dual_rumble_now), 走 250ms 播放
            // 路径的话 0/0 也会发满幅震动 (2026-10-20 持续嗡震根因).
            // duration=0 路径才发标准中性字节 {0,1,40,40}.
            if (s_rumble_weak == 0 && s_rumble_strong == 0)
                d->report_parser.play_dual_rumble(d, 0, 0, 0, 0);
            else
                d->report_parser.play_dual_rumble(d, 0, 250, s_rumble_weak, s_rumble_strong);
            logi("gate2-tx: rumble w=%d s=%d 已发\n", s_rumble_weak, s_rumble_strong);
        }
    }

    // 电池: Bluepad32 只在 0x21 ACK 时更新 -> 周期发无振动子命令触发应答
    s_data_calls++;
    if (s_data_calls % BAT_REFRESH_CALLS == 0 && d && d->report_parser.play_dual_rumble) {
        d->report_parser.play_dual_rumble(d, 0, 0, 0, 0);
    }

    // 诊断统计 (每 600 次 ~5s 一行, 现在 120Hz)
    for (int i = 0; i < 3; i++) {
        if (gp->gyro[i] > s_gmax[i]) s_gmax[i] = gp->gyro[i];
    }
    if (s_data_calls % 600 == 0) {
        logi("gate2-tx: n=%d bat=%d gyro[%7d %7d %7d] gMax[%7d %7d %7d]\n",
             s_data_calls, ctl->battery,
             gp->gyro[0], gp->gyro[1], gp->gyro[2],
             s_gmax[0], s_gmax[1], s_gmax[2]);
    }
}

static const uni_property_t* my_platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void my_platform_on_oob_event(uni_platform_oob_event_t event, void* data) {
    ARG_UNUSED(data);
    logi("gate2-tx: on_oob_event: 0x%04x\n", event);
}

//
// Entry Point
//
struct uni_platform* get_my_platform(void) {
    static struct uni_platform plat = {
        .name = "gate2-tx",
        .init = my_platform_init,
        .on_init_complete = my_platform_on_init_complete,
        .on_device_discovered = my_platform_on_device_discovered,
        .on_device_connected = my_platform_on_device_connected,
        .on_device_disconnected = my_platform_on_device_disconnected,
        .on_device_ready = my_platform_on_device_ready,
        .on_oob_event = my_platform_on_oob_event,
        .on_controller_data = my_platform_on_controller_data,
        .get_property = my_platform_get_property,
    };

    return &plat;
}
