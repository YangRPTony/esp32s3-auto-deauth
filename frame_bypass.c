/*
 * frame_bypass.c —— 解除 esp_wifi_80211_tx 的帧类型白名单
 *
 * ESP32-S3 esp32:esp32 3.3.12 的 libnet80211.a 内部有一个强符号
 * ieee80211_raw_frame_sanity_check()：它只放行 beacon / probe req /
 * probe resp / action / 非QoS数据帧，Deauth(FC 0xC0) 会被拒绝，
 * 串口报 "wifi:unsupport frame type: 0c0"，帧从未上天（这就是断网失效的根因）。
 *
 * 我们已把库里该符号的符号表绑定改为 WEAK（nm 验证 0000005c W）。
 * 链接器规则：强符号覆盖弱符号 —— 本文件这个强定义在链接时胜出，
 * 返回 0 = 放行任何帧。
 *
 * g_bypass_hits 记录本函数被调用的次数，固件串口打印它用来验证覆盖是否真的生效。
 * 还原方法：删掉本文件重新编译即可回到原厂库行为。
 */
#include <stdint.h>

volatile long g_bypass_hits = 0;

int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    g_bypass_hits++;
    return 0; /* 0 = 放行 */
}
