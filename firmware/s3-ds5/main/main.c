// gate2_ds5: S3 侧 - UART 收 ESP32 转发帧 -> 映射 -> TinyUSB DualSense (054C:0CE6)
// 架构: F1(NS蓝牙) -> 原版ESP32(Bluepad32) --UART 460800--> 本板 --USB--> PC
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "tinyusb.h"
#include "tusb.h"
#include "ds5_report.h"

#define RX_PIN 5           // 接 ESP32 GPIO25 (TX)
#define TX_PIN 4           // 预留: 回传振动/LED 到 ESP32 GPIO26 (RX)
#define UART_BAUD 921600
#define FRAME_LEN 44
#define STALE_MS 120       // 超过此时间无有效帧 -> 视为断流

static const char* TAG = "gate2";

// ---------- Bluepad32 位定义 (uni_gamepad.h) ----------
enum {
    BP_DPAD_UP = 1 << 0, BP_DPAD_DOWN = 1 << 1, BP_DPAD_RIGHT = 1 << 2, BP_DPAD_LEFT = 1 << 3,
};
enum {
    BP_BUTTON_A = 1 << 0, BP_BUTTON_B = 1 << 1, BP_BUTTON_X = 1 << 2, BP_BUTTON_Y = 1 << 3,
    BP_SHOULDER_L = 1 << 4, BP_SHOULDER_R = 1 << 5,
    BP_TRIGGER_L = 1 << 6, BP_TRIGGER_R = 1 << 7,
    BP_THUMB_L = 1 << 8, BP_THUMB_R = 1 << 9,
};
enum {
    BP_MISC_SYSTEM = 1 << 0, BP_MISC_SELECT = 1 << 1, BP_MISC_START = 1 << 2, BP_MISC_CAPTURE = 1 << 3,
};

// ---------- 共享状态 (SPSC 环形队列: UART 任务生产, USB 任务消费) ----------
#define RING_N 8
static ds5_report_t s_ring[RING_N];
static volatile int s_head = 0;   // 写指针 (仅 uart_task 改)
static volatile int s_tail = 0;   // 读指针 (仅 usb_task 改; 满时 uart_task 代推)
static ds5_report_t s_last_sent;  // 空闲保活重发用
static volatile bool s_connected = false;
static volatile int64_t s_last_frame_us = 0;
static volatile uint32_t s_frames_ok = 0, s_frames_bad = 0;
static uint8_t s_seq = 0;
static uint32_t s_sensor_ts = 0;
static volatile uint8_t s_last_batt = 0;  // 最近一帧 TX 电量(诊断用, 0=未知)
static TaskHandle_t s_usb_taskh = NULL;   // ring_push 通知发送任务立即唤醒

static void ring_push(const ds5_report_t* r) {
    int next = (s_head + 1) % RING_N;
    if (next == s_tail)              // 满: 丢最旧 (USB 侧卡住时别堵 UART)
        s_tail = (s_tail + 1) % RING_N;
    s_ring[s_head] = *r;
    s_head = next;
    if (s_usb_taskh) xTaskNotifyGive(s_usb_taskh);   // 立即唤醒发送 (省最多 1ms)
}

static uint8_t crc8(const uint8_t* p, int len) {
    uint8_t c = 0;
    for (int i = 0; i < len; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

static int16_t rd_i16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }
static int32_t rd_i32(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Bluepad32 dpad 位 -> DS5 hat 编码
static uint8_t dpad_to_hat(uint8_t dpad) {
    bool u = dpad & BP_DPAD_UP, d = dpad & BP_DPAD_DOWN;
    bool l = dpad & BP_DPAD_LEFT, r = dpad & BP_DPAD_RIGHT;
    if (u && r) return DS5_HAT_NE;
    if (r && d) return DS5_HAT_SE;
    if (d && l) return DS5_HAT_SW;
    if (l && u) return DS5_HAT_NW;
    if (u) return DS5_HAT_N;
    if (r) return DS5_HAT_E;
    if (d) return DS5_HAT_S;
    if (l) return DS5_HAT_W;
    return DS5_HAT_RELEASED;
}

// ---- 陀螺静止零偏跟踪 ----
// v1 (相邻样本<3dps 门): F1 噪声 3-8dps, 永不触发.
// v2 (相邻<8dps + 偏差<25dps 门): 会把 <25dps 的真实慢转当静止吃进零偏 ——
//    慢转相邻样本变化极小 (20dps 慢转每帧仅 ~0.1dps), 2026-10-21 晚实测:
//    27° 慢翻滚积分只剩 -1.1° ("慢动作没反应/静止乱转"的根源).
// v3 窗口均值式: 按 92 样本 (~0.5s @185fps) 窗口求均值判静止 ——
//    窗口均值 |速率|<4dps 且 相邻窗口加计均值差<15 counts (~4mg).
//    噪声零均值 -> 窗口均值稳定, 可吸; 真实慢转 -> 均值速率超门或加计均值
//    单调移动 (0.7dps 转动 0.5s 即移动 36 counts) 被拦.
//    吸收: 前 2s 静止 EMA/32 慢吸 (防误吃), 连续静止 4 窗后 EMA/4 快吸
//    (重连后克隆重校零失败的大偏置 ~2-3s 拉平).
static int32_t s_gbias_mdps[3];
static int32_t s_wsum_g[3];
static int32_t s_wsum_a[3];
static int32_t s_prev_mean_a[3];
static uint16_t s_wcount = 0;
static uint8_t s_still_windows = 0;
// 克隆加计零偏 (raw Switch counts, 4096c/g). 每次重连/唤醒克隆自行重校零且
// 不稳定 (2026-10-21 同日实测 +0.16g -> -0.29g), 硬编码打地鼠 -> 运行时自适应:
// 种子=当日实测; 平放长静止 (连续静止>=8s 且 |ay|>0.93g) 时向纯重力 (0,±1g,0)
// 收敛, 限幅 64counts/0.5s (0.45g 野偏 ~20s 自愈; 手持瞄准停顿 <8s 或有微动,
// 不会误触发; 真实桌面小倾角会被吃掉一点, 上限即桌倾 ~几度, 远好于克隆野偏).
static int32_t s_aoff_raw[3] = {1212, -65, -182};
#define BIAS_WIN 92
static void gyro_bias_track(const int32_t g[3], const int16_t a[3]) {
    // a[] 为未扣零偏的原始加计: 静止判据 (admax) 必须看原始值, 否则自适应每走
    // 一步 (64 counts) 都会把"窗口均值变化"顶破 15 counts 门, 静止计数清零,
    // 8.5s 才走一步 (实测收敛慢 30 倍的根因).
    for (int i = 0; i < 3; i++) {
        s_wsum_g[i] += g[i];
        s_wsum_a[i] += a[i];
    }
    if (++s_wcount < BIAS_WIN) return;
    s_wcount = 0;
    int32_t mean_g[3], mean_a[3], dmax = 0, admax = 0;
    for (int i = 0; i < 3; i++) {
        mean_g[i] = s_wsum_g[i] / (int32_t)BIAS_WIN;
        s_wsum_g[i] = 0;
        mean_a[i] = s_wsum_a[i] / (int32_t)BIAS_WIN;
        s_wsum_a[i] = 0;
        int32_t d = mean_g[i];
        if (d < 0) d = -d;
        if (d > dmax) dmax = d;
        d = mean_a[i] - s_prev_mean_a[i];
        if (d < 0) d = -d;
        if (d > admax) admax = d;
        s_prev_mean_a[i] = mean_a[i];
    }
    if (dmax < 4000 && admax < 15) {
        if (s_still_windows < 32) s_still_windows++;
        int32_t div = (s_still_windows >= 4) ? 4 : 32;
        for (int i = 0; i < 3; i++)
            s_gbias_mdps[i] += (mean_g[i] - s_gbias_mdps[i]) / div;
        // 平放长静止: 加计零偏向纯重力收敛 (限幅防抖).
        // 轴序: a[] 是 Switch 原始轴 (0=鼻 1=左 2=正面), 平放重力在 a2!
        // 用扣偏后的值对目标收敛.
        int32_t ca[3];
        for (int i = 0; i < 3; i++) ca[i] = mean_a[i] - s_aoff_raw[i];
        if (s_still_windows >= 16 && (ca[2] > 3810 || ca[2] < -3810)) {
            int32_t tgt[3] = {0, 0, (ca[2] >= 0) ? 4096 : -4096};
            for (int i = 0; i < 3; i++) {
                int32_t corr = (ca[i] - tgt[i]) / 4;
                if (corr > 64) corr = 64;
                if (corr < -64) corr = -64;
                s_aoff_raw[i] += corr;
            }
        }
    } else {
        s_still_windows = 0;
    }
}

// 死区: |v|<=4dps 清零, 4~9dps 线性过渡, 以上原样 (滤 F1 底噪+残余偏置+手抖级噪声)
static inline int32_t gyro_deadband_mdps(int32_t v) {
    int32_t a = v < 0 ? -v : v;
    if (a <= 4000) return 0;
    if (a < 9000) {
        int32_t r = (a - 4000);
        return v < 0 ? -r : r;
    }
    return v;
}

// v5.1 抓包遥测: PC 发来的 0x02 输出报告摘要, 塞进 0x01 的保留尾字节 [56..63].
// Windows HID 输入报告会广播给所有句柄 -> PC 上 Python 可并行读取, 游戏/Steam
// 不解析这些保留字节, 不影响手柄功能. 布局:
// [56]=构建标记 0x53(v5.3)  [57]=0x02包序号(每包+1)  [58]=EnableBits1  [59]=EnableBits2
// [60]=weak(b3)  [61]=strong(b4)  [62]=EnableBits3(b39)  [63]=R2扳机效果类型(b11)
static volatile uint8_t s_out_dbg[8] = {0x54, 0, 0, 0, 0, 0, 0, 0};
static volatile uint8_t s_out_seq = 0;

// 帧解析 -> 填 s_next (不发送)
static void apply_frame(const uint8_t* f) {
    uint32_t buttons = (uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
    uint16_t brake = f[8] | (f[9] << 8);
    uint16_t throttle = f[10] | (f[11] << 8);
    uint8_t dpad = f[40], misc = f[41];
    uint8_t battery = f[39];

    ds5_report_t r_buf;
    ds5_report_t* r = &r_buf;
    ds5_report_init(r);

    // 摇杆/扳机
    r->lx = ds5_axis_i16_to_u8(rd_i16(f + 12));
    r->ly = ds5_axis_i16_to_u8(rd_i16(f + 14));
    r->rx = ds5_axis_i16_to_u8(rd_i16(f + 16));
    r->ry = ds5_axis_i16_to_u8(rd_i16(f + 18));
    r->l2 = brake >> 2;   // 0..1023 -> 0..255
    r->r2 = throttle >> 2;
    // NS 协议扳机只有数字量 (brake/throttle 恒 0): 按数字位合成满行程,
    // 否则主机读模拟字节恒 0, 游戏里 L2/R2 无效
    if (r->l2 == 0 && (buttons & BP_TRIGGER_L)) r->l2 = 255;
    if (r->r2 == 0 && (buttons & BP_TRIGGER_R)) r->r2 = 255;

    // 扳机反馈: 不做人工合成震动. 游戏真实的开火后坐力震动经 forward_rumble
    // 转发, 那才是"有反馈才震" (2026-10-20 用户定案, 人工脉冲已删).

    // 按键: F1 在 NS 档按丝印逻辑上报 + Bluepad32 任天堂标签透传
    // (Switch 逻辑 A=BP_B, B=BP_A, X=BP_Y, Y=BP_X), PC 用户期望丝印走 Xbox 语义:
    // 丝印 A->CROSS, B->CIRCLE, X->SQUARE, Y->TRIANGLE (2026-10-21 用户实测"反了"修正)
    uint8_t b0 = dpad_to_hat(dpad);
    if (buttons & BP_BUTTON_Y) b0 |= 1 << 4;  // Switch 逻辑X(丝印X,西) -> SQUARE
    if (buttons & BP_BUTTON_B) b0 |= 1 << 5;  // Switch 逻辑A(丝印A,南) -> CROSS
    if (buttons & BP_BUTTON_A) b0 |= 1 << 6;  // Switch 逻辑B(丝印B,东) -> CIRCLE
    if (buttons & BP_BUTTON_X) b0 |= 1 << 7;  // Switch 逻辑Y(丝印Y,北) -> TRIANGLE
    r->buttons[0] = b0;

    uint8_t b1 = 0;
    if (buttons & BP_SHOULDER_L) b1 |= 1 << 0;
    if (buttons & BP_SHOULDER_R) b1 |= 1 << 1;
    if (buttons & BP_TRIGGER_L) b1 |= 1 << 2;
    if (buttons & BP_TRIGGER_R) b1 |= 1 << 3;
    if (misc & BP_MISC_START) b1 |= 1 << 5;    // + -> OPTIONS
    if (buttons & BP_THUMB_L) b1 |= 1 << 6;
    if (buttons & BP_THUMB_R) b1 |= 1 << 7;
    r->buttons[1] = b1;

    uint8_t b2 = 0;
    if (misc & BP_MISC_SYSTEM) b2 |= 1 << 0;    // HOME -> PS (直通保留)
    if (misc & BP_MISC_CAPTURE) b2 |= 1 << 1;   // 截图 -> TOUCHPAD 直通 (还原)
    // 左侧功能键(SELECT/-): 短按=TOUCHPAD 点击(释放时合成 80ms), 长按 >=500ms
    // = 原功能 CREATE 按住 (2026-10-21 用户定案: PS 键会触发 Steam 模式弃用,
    // 截图键长按=PS 方案已还原)
    {
        static bool s_sel_down = false;
        static bool s_sel_long = false;
        static int64_t s_sel_down_us = 0;
        static int64_t s_tp_click_until_us = 0;
        int64_t sel_now_us = esp_timer_get_time();
        bool sel_now = (misc & BP_MISC_SELECT) != 0;
        if (sel_now && !s_sel_down) {
            s_sel_down = true;
            s_sel_long = false;
            s_sel_down_us = sel_now_us;
        } else if (!sel_now && s_sel_down) {
            s_sel_down = false;
            if (!s_sel_long) s_tp_click_until_us = sel_now_us + 80000;
        }
        if (s_sel_down && !s_sel_long && sel_now_us - s_sel_down_us >= 500000)
            s_sel_long = true;
        if (s_sel_long && s_sel_down) b1 |= 1 << 4;
        if (sel_now_us < s_tp_click_until_us) b2 |= 1 << 1;
    }
    r->buttons[2] = b2;

    // IMU: F1(Switch Pro 克隆) 轴 -> DS5 轴.
    // 定案(2026-10-21): 直接采用 SDL hidapi_switch.c SendSensorUpdate 的
    // Switch->PlayStation 官方变换:
    //   X_ds5 = -Y_sw, Y_ds5 = +Z_sw, Z_ds5 = -X_sw (det=+1)
    // 克隆 0x30 原始帧 == 真实 Pro 帧(X_sw=鼻/顶部, Y_sw=左侧, Z_sw=出正面,
    // 右手系) —— 重力姿态与官方变换互推一致; 早先"克隆旋转90度装贴"理论
    // 有误, 三代固件的"符号翻翻翻"全是 X/Z 轴指派接反的衍生症状:
    // v3 实测 翻滚<->俯仰显示对调 = 轴指派错的直接证据.
    // 验证: 骨牌P1 抬鼻65度 -> 原始 Y_sw 积分 -64 -> out_X=+64=DS5抬头正方向;
    // Y 行 +Z_sw 三版未动, 水平转始终正确; DS5 物理约定 X=右,Y=出正面,
    // Z=朝玩家, 加计重力姿态按此矩阵自动吻合(平放+Y, 立鼻-Z, 左侧立-X).
    int16_t asw[3];
    int32_t gsw[3];
    for (int i = 0; i < 3; i++) {
        asw[i] = rd_i16(f + 20 + i * 2);
        gsw[i] = rd_i32(f + 26 + i * 4);
    }
    // 静止检测用原始加计 (见 gyro_bias_track 注释)
    gyro_bias_track(gsw, asw);
    // 加计零偏 (raw 域, 自适应) 扣除后再进输出矩阵
    for (int i = 0; i < 3; i++) {
        int32_t c = (int32_t)asw[i] - s_aoff_raw[i];
        if (c > 32767) c = 32767;
        if (c < -32768) c = -32768;
        asw[i] = (int16_t)c;
    }
    for (int i = 0; i < 3; i++) gsw[i] = gyro_deadband_mdps(gsw[i] - s_gbias_mdps[i]);
    r->accel[0] = ds5_accel_from_switch((int16_t)-asw[1]);
    r->accel[1] = ds5_accel_from_switch(asw[2]);
    r->accel[2] = ds5_accel_from_switch((int16_t)-asw[0]);
    r->gyro[0] = ds5_gyro_from_mdps(-gsw[1]);
    r->gyro[1] = ds5_gyro_from_mdps(gsw[2]);
    r->gyro[2] = ds5_gyro_from_mdps(-gsw[0]);
    // 时间戳: 真 DS5 单位 = 0.33us (内核 hid-playstation: timestamp/3 = us).
    // v5.4 的"突发等间隔化"实测产生 dt=0/25ms 锯齿(Steam 融合器爆掉、左右翻失控
    // 的根因, 2026-10-21 夜) 已回退 —— 墙钟差值版经 v5.3 全量验证无害.
    static int64_t s_last_ts_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (s_last_ts_us) s_sensor_ts += (uint32_t)((now_us - s_last_ts_us) * 3);
    s_last_ts_us = now_us;
    r->ts = s_sensor_ts;

    // 电池: TX 侧 Bluepad32 语义 0=未知 1=空 64/128/192=档位 255=满
    // (旧代码把 255 当"无上报"显示中档, 满电被吃掉). DS5 编码(SDL ucBatteryLevel):
    // 低4位 level 0-10 (percent=level*10+5), 0x0C="不报电量", 高4位 0=放电.
    s_last_batt = battery;
    uint8_t cap = (battery == 0) ? 5 : (uint8_t)((battery * 10 + 127) / 255);
    if (cap > 10) cap = 10;
    r->status[0] = cap;
    r->status[1] = 0x08;  // 连接状态: 0x08=USB (SDL 注释, 真有线 DS5 样貌)

    r->seq = 0;  // 序号统一由 usb_task 发送时打

    // v5.1 遥测: 最近的 0x02 输出报告摘要进保留尾字节 (见 s_out_dbg)
    for (int i = 0; i < 8; i++) r->reserved4[i] = s_out_dbg[i];

    // 入队, 通知发送
    ring_push(r);
    s_connected = (f[38] & 1) != 0;
    s_last_frame_us = esp_timer_get_time();
}

// 断流保护: gyro 清零, 摇杆回中, 按键释放 (陀螺仪悬停非零会导致视角持续漂移)
static void apply_stale(void) {
    ds5_report_t r;
    ds5_report_init(&r);
    ring_push(&r);
}

// ---------- UART 解析任务 ----------
static void uart_task(void* arg) {
    esp_task_wdt_add(NULL);   // 死锁 5s -> PANIC 重启自愈 (sdkconfig)
    const int rb = 2048;
    uart_driver_install(UART_NUM_1, rb * 2, 0, 0, NULL, 0);
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
    // 振动回传走 UART1 TX(GPIO4); 镜像到 GPIO6 (用户焊接脚可能是 6), 两脚同时输出
    esp_rom_gpio_connect_out_signal(6, U1TXD_OUT_IDX, false, false);

    uint8_t buf[256];
    uint8_t frame[FRAME_LEN];
    int flen = 0;      // 半帧缓存长度
    bool in_frame = false;
    ESP_LOGI(TAG, "UART1 RX=GPIO%d @%d, 等 ESP32 帧...\n", RX_PIN, UART_BAUD);

    while (1) {
        esp_task_wdt_reset();   // 喂狗: 循环卡死 5s -> 重启自愈
        int n = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (!in_frame) {
                // 找帧头: A5 5A
                static uint8_t prev = 0;
                if (prev == 0xA5 && c == 0x5A) {
                    in_frame = true;
                    frame[0] = 0xA5; frame[1] = 0x5A;
                    flen = 2;
                }
                prev = c;
                continue;
            }
            frame[flen++] = c;
            if (flen == 3 && frame[2] != 0 && frame[2] != 0xFF) {
                // seq 字节任意, 继续读 len
            }
            if (flen == 4 && frame[3] != FRAME_LEN) {
                in_frame = false;  // 长度不对, 重新找头
                continue;
            }
            if (flen == FRAME_LEN) {
                in_frame = false;
                if (crc8(frame, FRAME_LEN - 1) == frame[FRAME_LEN - 1]) {
                    s_frames_ok++;
                    apply_frame(frame);
                } else {
                    s_frames_bad++;
                }
            }
        }
        // 断流检测
        if (s_last_frame_us && esp_timer_get_time() - s_last_frame_us > STALE_MS * 1000) {
            apply_stale();
            s_last_frame_us = 0;
        }
        // 每 5s 打印收帧统计: ok=0 说明 UART 没通 (查接线/波特率)
        static uint32_t last_stats_ms = 0;
        uint32_t now_ms = esp_log_timestamp();
        if (now_ms - last_stats_ms > 5000) {
            ESP_LOGI(TAG, "UART stats: ok=%lu bad=%lu bat=%u%s", (unsigned long)s_frames_ok,
                     (unsigned long)s_frames_bad, (unsigned)s_last_batt,
                     s_frames_ok ? "" : "  <- 无帧! 查 ESP32 GPIO25->S3 GPIO5 和共地");
            last_stats_ms = now_ms;
        }
    }
}

// ---------- USB 发送任务 ----------
// 注意: esp_tinyusb 组件默认自建任务跑 tud_task() (sdkconfig: NO_DEFAULT_TASK 未置位),
// 这里不能再调 tud_task(), 否则两个消费者抢事件队列, 本任务会永久阻塞.
// 优化 (2026-10-21): ① 空闲时阻塞在任务通知上(ring_push 唤醒), 取代固定 1ms 轮询,
//   每帧省最多 1ms 延迟; ② 订阅任务看门狗(sdkconfig PANIC=y, 死锁→重启自愈);
//  ③ 数据积压 + not_ready 且主机未挂起持续 8s = TinyUSB 状态机疑卡死 → esp_restart
//   (栈任务(tud_task)本身卡死时看门狗抓不到, 用积压判据兜底; 主机挂起(tud_suspended)
//   是合法状态不重启).
static void usb_task(void* arg) {
    static int64_t last_send_us = 0;
    static uint32_t tx_count = 0, ready_false = 0;
    esp_task_wdt_add(NULL);
    int64_t stall_since_us = 0;
    while (1) {
        esp_task_wdt_reset();
        bool pending = (s_tail != s_head);
        if (pending) {
            if (tud_hid_n_ready(0)) {
                ds5_report_t out;
                memcpy(&out, (void*)&s_ring[s_tail], sizeof(out));
                out.seq = s_seq++;
                if (tud_hid_n_report(0, 0, &out, sizeof(out))) {
                    s_last_sent = out;
                    s_tail = (s_tail + 1) % RING_N;
                    tx_count++;
                    last_send_us = esp_timer_get_time();
                    stall_since_us = 0;
                } else {
                    ready_false++;
                }
                vTaskDelay(1);           // EP 间隔 1ms 自然限速
            } else {
                ready_false++;
                if (!tud_suspended()) {  // 积压+忙 等 8s = 疑似栈卡死
                    int64_t now = esp_timer_get_time();
                    if (!stall_since_us) stall_since_us = now;
                    if (now - stall_since_us > 8000000) {
                        ESP_LOGE(TAG, "USB 栈疑卡死(积压+not_ready 8s), 重启自愈");
                        esp_restart();
                    }
                } else {
                    stall_since_us = 0;  // 主机挂起是合法状态
                }
                vTaskDelay(1);
            }
        } else if (esp_timer_get_time() - last_send_us > 10000) {
            // 空闲保活: >10ms 无数据重发上一帧 (主机靠周期报告判断手柄在线)
            if (tud_hid_n_ready(0)) {
                ds5_report_t out = s_last_sent;
                out.seq = s_seq++;
                if (tud_hid_n_report(0, 0, &out, sizeof(out))) {
                    tx_count++;
                    last_send_us = esp_timer_get_time();
                }
            }
            vTaskDelay(1);
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));  // 等新帧, 立即唤醒
        }
        static uint32_t last_stat_ms = 0;
        if (esp_log_timestamp() - last_stat_ms > 5000) {
            ESP_LOGI(TAG, "USB tx=%lu not_ready=%lu", (unsigned long)tx_count, (unsigned long)ready_false);
            last_stat_ms = esp_log_timestamp();
        }
    }
}

// ---------- TinyUSB HID 回调 ----------
// feature 0x05: DS5 出厂 IMU 校准. 数值必须满足 SDL 的契约与合法性检查
// (sdl_ps5.c: sens=(speed_p+speed_m)*1024/(plus-minus); 校准后单位 1024 c/dps;
//  合法窗口: gyro sens≈64, accel sens≈1, |bias|<=1024, 否则整份校准被丢弃):
//   gyro: speed=512/512(和1024), plus-minus=8192-(-8192)=16384 -> sens=64.0 精确
//   accel: ±8192(差16384) -> sens=2*8192/16384=1.0, bias=plus-range/2=0
// 配合原始刻度 16 c/dps: 校准路径 (raw-0)*64 与无校准回退 raw*64 输出完全一致.
// 历史: 全零版 0/0=NaN 骗过合法性检查把 NaN 灌给主机; sens=1.0 版被判非法落入回退,
// 而旧原始刻度 64c/dps 使 Steam 读出 4 倍速度 ("动一下就是最高速").
// 注意: 回调 buffer 不含 report id(协议栈自动前置), 字段从 buffer[0] 开始;
// 其他 feature(0x0120 等)维持全零应答(全程被 Steam 容忍).
static void put_le16(uint8_t* p, int16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t* buffer, uint16_t reqlen) {
    (void)itf;
    memset(buffer, 0, reqlen);
    if (report_type != HID_REPORT_TYPE_FEATURE)
        return reqlen;
    if (report_id == 0x05) {
        // gyro bias (pitch/yaw/roll) [0..5] = 0 (固件内 bias_track 已归零)
        for (int i = 0; i < 3; i++) {
            put_le16(&buffer[6 + i * 4], 8192);   // plus
            put_le16(&buffer[8 + i * 4], -8192);  // minus
        }
        put_le16(&buffer[18], 512);  // speed plus
        put_le16(&buffer[20], 512);  // speed minus
        for (int i = 0; i < 3; i++) {
            put_le16(&buffer[22 + i * 4], 8192);   // accel plus
            put_le16(&buffer[24 + i * 4], -8192);  // accel minus
        }
    } else if (report_id == 0x20) {
        // 固件信息 (Linux hid-playstation dualsense_get_firmware_info, buf 含 id:
        // hw LE32@24/fw LE32@28/update LE16@44 -> 本回调 buffer 前移 1).
        // 全零曾被 Steam 判为未知设备 -> 陀螺仪不做校准直读(实测 16 倍过灵敏).
        // 样貌升级 (2026-10-21): update 字段 = 当前最新固件 0x0630
        // (Paliverse/DualSense-List-of-Firmwares: Type 0004/000B 最新 0x0630);
        // 旧样貌 2.21 < 2.24 增强功能门槛, COD 原生体感检测报"半支持"疑与此
        // 相关; SDL 以 update 字段(本行 buffer[43..44])为 firmware_version,
        // >=0x0224 走增强震动路径(转发已兼容).
        put_le16(&buffer[23], 0x0514); put_le16(&buffer[25], 0x0000);
        put_le16(&buffer[27], 0x000a); put_le16(&buffer[29], 0x0300);
        put_le16(&buffer[43], 0x0630);
    } else if (report_id == 0x09) {
        // 配对信息: MAC @buf[1..6] -> buffer[0..5], 与 USB 序列号一致
        buffer[0] = 0x94; buffer[1] = 0xCE; buffer[2] = 0x2A;
        buffer[3] = 0x11; buffer[4] = 0x22; buffer[5] = 0x33;
    }
    return reqlen;
}

// DS5 0x02 输出报告 -> F1 振动转发.
// TinyUSB OUT 端点回调 buffer 含报告 id(b[0]=0x02), effects 从 b[1] 起
// (SDL DS5EffectsState_t): b[1]EnableBits1(0x01=经典振动,0x02=关音频触觉)
// b[2]EnableBits2(0x04=灯色 0x10=玩家灯) b[3]RumbleRight(高频) b[4]RumbleLeft
// b[5..8]音量/音频位 b[11..21]右扳机效果 b[22..32]左扳机效果 b[39]EnableBits3(0x04=增强振动).
// 振动字节必须由 enable 位门控: Steam 常驻的音量/灯光帧这些字节非零,
// 旧"峰值代理"把音量当振动 -> 一直在震(2026-10-21 实测). 音量/音频不转发.
// 地板值: 游戏常发的低强度振动(真 DS5 马达上只是轻微质感)在 F1 上是嗡震,
// 低于此值直接归零, 只转发真实反馈级别的强度. 人工合成震动已全部移除.
#define RUMBLE_FLOOR 70
static void forward_rumble(const uint8_t* b, int len) {
    if (len < 5) return;
    int weak = 0, strong = 0;
    bool rumble_on = (b[1] & 0x01) != 0 || (len > 39 && (b[39] & 0x04) != 0);
    if (rumble_on) {
        weak = b[3];
        strong = b[4];
        if (weak < RUMBLE_FLOOR) weak = 0;
        if (strong < RUMBLE_FLOOR) strong = 0;
    }
    static int s_last_weak = -1, s_last_strong = -1;
    static int64_t s_last_fwd_us = 0;
    int64_t now = esp_timer_get_time();
    bool active = (weak || strong) != 0;
    // 静止: 一个字节都不发. 停止也不发 R0,0 —— TX 侧 rumble 自带 250ms 定时
    // 自动停 (中性字节), 而 Bluepad32 的 switch rumble 编码幅值写死 500, 任何
    // R 命令 (包括 0,0) 都会触发一次满幅震. 旧空闲保活 -> 一进游戏 0x02 包流
    // 到来就以 10Hz 连发 R0,0 = 持续嗡震 (2026-10-20 遥测抓包定案).
    // 激活: 每 200ms 重发维持 (TX 单次 250ms 自停).
    if (!active) {
        s_last_weak = 0;
        s_last_strong = 0;
        return;
    }
    if (weak == s_last_weak && strong == s_last_strong &&
        now - s_last_fwd_us < 200000)
        return;
    s_last_weak = weak;
    s_last_strong = strong;
    s_last_fwd_us = now;
    char cmd[16];
    int n = snprintf(cmd, sizeof(cmd), "R%d,%d\n", weak, strong);
    uart_write_bytes(UART_NUM_1, cmd, n);
    ESP_LOGI(TAG, "振动转发: weak=%d strong=%d", weak, strong);
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t* buffer, uint16_t bufsize) {
    // PC -> 手柄: 0x02 输出报告 (振动/灯/扳机效果), OUT 端点路径 buffer 含 id 于 [0]
    // 扳机效果字节不解析不转发: F1 无自适应扳机硬件, 且 Steam 连接时常驻扳机效果,
    // 无法据此区分"游戏真实反馈" (2026-10-20 用户定案: 只转发真实震动).
    (void)itf; (void)report_type;
    if (report_id == 0x02 || (report_id == 0 && bufsize > 0 && buffer[0] == 0x02)) {
        forward_rumble(buffer, bufsize);
        // v5.1 抓包遥测: 记录本包摘要 (字节含义见 s_out_dbg 注释)
        s_out_seq++;
        s_out_dbg[1] = s_out_seq;
        s_out_dbg[2] = (bufsize > 1) ? buffer[1] : 0;
        s_out_dbg[3] = (bufsize > 2) ? buffer[2] : 0;
        s_out_dbg[4] = (bufsize > 3) ? buffer[3] : 0;
        s_out_dbg[5] = (bufsize > 4) ? buffer[4] : 0;
        s_out_dbg[6] = (bufsize > 39) ? buffer[39] : 0;
        s_out_dbg[7] = (bufsize > 11) ? buffer[11] : 0;
    }
}

void tud_mount_cb(void) { ESP_LOGI(TAG, "USB 已挂载 (PC 识别中)"); }
void tud_umount_cb(void) { ESP_LOGI(TAG, "USB 已拔出"); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; ESP_LOGI(TAG, "USB 挂起"); }
void tud_resume_cb(void) { ESP_LOGI(TAG, "USB 恢复"); }

// ---------- app_main ----------
void app_main(void) {
    ESP_LOGI(TAG, "=== gate2_ds5: ESP32-S3 DualSense 桥 ===");
    ESP_LOGI(TAG, "接线: ESP32 GPIO25(TX) -> S3 GPIO%d(RX), GND 共地", RX_PIN);
    ESP_LOGI(TAG, "USB: 本板原生口 -> PC (识别为 054C:0CE6 DualSense)");

    ds5_report_init(&s_last_sent);

    // TinyUSB 初始化 (DS5 描述符, 见 usb_descriptors.c)
    extern const tusb_desc_device_t ds5_device_descriptor;
    extern const uint8_t ds5_configuration_descriptor[];
    extern const char* ds5_string_descriptor[];
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &ds5_device_descriptor,
        .string_descriptor = ds5_string_descriptor,
        .string_descriptor_count = 4,
        .external_phy = false,
        .configuration_descriptor = ds5_configuration_descriptor,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    xTaskCreatePinnedToCore(uart_task, "uart", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(usb_task, "usb", 4096, NULL, 11, &s_usb_taskh, 0);  // 11: 通知即时抢占
}
