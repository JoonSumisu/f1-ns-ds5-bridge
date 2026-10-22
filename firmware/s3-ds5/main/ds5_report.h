// DualSense USB 输入报告 (0x01, 64B) 布局与填充助手
// 依据: Linux hid-playstation.c + nondebug/dualsense 文档
#ifndef DS5_REPORT_H
#define DS5_REPORT_H

#include <stdint.h>

// 64 字节完整报告 (含 report id)
typedef struct __attribute__((packed)) {
    uint8_t report_id;      // [0]  = 0x01
    uint8_t lx, ly;         // [1,2]   左摇杆 0..255, 中位 128
    uint8_t rx, ry;         // [3,4]   右摇杆
    uint8_t l2, r2;         // [5,6]   扳机 0..255
    uint8_t seq;            // [7]     序号
    uint8_t buttons[4];     // [8..11]
    uint8_t reserved[4];    // [12..15]
    int16_t gyro[3];        // [16..21] x,y,z (LE), 16 counts/dps (原始刻度, 校准后 1024/dps)
    int16_t accel[3];       // [22..27] x,y,z (LE), ~8192 counts/g
    uint32_t ts;            // [28..31] 传感器时间戳
    uint8_t reserved2;      // [32]
    uint8_t touch[8];       // [33..40] 两个触点
    uint8_t reserved3[12];  // [41..52]
    uint8_t status[3];      // [53..55] status[0]: 低4位电池, 高4位充电
    uint8_t reserved4[8];   // [56..63]
} ds5_report_t;

_Static_assert(sizeof(ds5_report_t) == 64, "ds5_report_t must be 64 bytes");

// buttons[0]: 低4位 = hat (0=N,1=NE,...,7=NW, 8=松开); bit4=SQUARE bit5=CROSS bit6=CIRCLE bit7=TRIANGLE
// buttons[1]: bit0=L1 1=R1 2=L2 3=R2 4=CREATE 5=OPTIONS 6=L3 7=R3
// buttons[2]: bit0=PS 1=TOUCHPAD 2=MIC_MUTE

#define DS5_HAT_N 0
#define DS5_HAT_NE 1
#define DS5_HAT_E 2
#define DS5_HAT_SE 3
#define DS5_HAT_S 4
#define DS5_HAT_SW 5
#define DS5_HAT_W 6
#define DS5_HAT_NW 7
#define DS5_HAT_RELEASED 8

static inline void ds5_report_init(ds5_report_t* r) {
    // 全零后填中位值
    r->report_id = 0x01;
    r->lx = r->ly = r->rx = r->ry = 128;
    r->l2 = r->r2 = 0;
    r->buttons[0] = DS5_HAT_RELEASED;   // hat 松开 = 8
    r->buttons[1] = 0;
    r->buttons[2] = 0;
    r->buttons[3] = 0;
    // 触点: bit7=1 表示未触摸
    r->touch[0] = 0x80; r->touch[4] = 0x80;
}

// i16 (-512..511) -> u8 (0..255, 中位128; 四舍五入, 中心=128 不偏 127)
static inline uint8_t ds5_axis_i16_to_u8(int16_t v) {
    int32_t t = ((int32_t)v + 512) * 255 + 511;
    t /= 1023;
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    return (uint8_t)t;
}

// accel: Switch counts(4096/g) -> DS5 counts(8192/g) = x2
static inline int16_t ds5_accel_from_switch(int16_t sw) {
    int32_t v = (int32_t)sw * 2;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

// gyro: mdps -> DS5 counts (16 counts/dps)
// 链路: BP 裸路径 mdps = raw×1000. Switch 生态全线用 STMicro ×1.15 修正后的有效刻度
// (名义 16.384 c/dps ÷1.15): Linux hid-nintendo JC_IMU_GYRO_RES_PER_DPS=14247、
// SDL hidapi_switch.c:60 SWITCH_GYRO_SCALE=14.2842、Bluepad32 同源 —— 用户 NS 模式
// 直连 PC 时的手感即 raw/14.284, PS5 转译必须对齐它 (16.384 会低 14.7%, 实测"手感不对").
// counts = raw×16/14.2842 = mdps/893 (有效刻度 14.288, 与 SDL 差 0.03%).
// (桌面定角积分物理真值 ~15.5 c/dps, 即本常数比物理高 ~9% —— 刻意, 为匹配 NS 直连基准)
static inline int16_t ds5_gyro_from_mdps(int32_t mdps) {
    int32_t v = mdps / 893;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

#endif  // DS5_REPORT_H
