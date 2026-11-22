# ESP32-S3 自动断网固件 (Auto Deauth)

ESP32-S3 上的 802.11 deauthentication / disassociation 帧发射器，Arduino 框架编写，通过修改 ESP-IDF 预编译库的符号绑定绕过 `esp_wifi_80211_tx()` 的帧白名单检查。

> **仅限在你自己拥有或已获得书面授权的网络环境中、用于设备兼容性与漫游行为测试。** 对他人网络使用在绝大多数司法辖区都属于违法行为。使用者自行承担全部责任。

---

## 目录

- [适用芯片](#适用芯片)
- [最终产出（烧什么）](#最终产出烧什么)
- [烧录步骤](#烧录步骤)
  - [方式一：Flash Download Tool（图形界面）](#方式一flash-download-tool图形界面)
  - [方式二：esptool 命令行](#方式二esptool-命令行)
  - [方式三：Arduino IDE / arduino-cli](#方式三arduino-ide--arduino-cli)
- [版本更新](#版本更新)
- [工作原理](#工作原理)
- [使用边界](#使用边界)
- [编译](#编译)

---

## 适用芯片

| 项目 | 要求 |
|---|---|
| **芯片** | **ESP32-S3**（QFN56，rev v0.2 / v0.1 均可） |
| Flash | **≥ 4 MB**。分区表按 4 MB 布局；8 MB / 16 MB 的模组可直接刷，后 4 MB 空着 |
| PSRAM | 不需要（固件不使用 PSRAM） |
| 晶振 | 40 MHz |
| 串口 | CH340 / CH343 / CP2102 等 USB-UART 桥，或芯片原生 USB |
| 参考模组 | ESP32-S3-WROOM-1 / N8R2 / N16R8、ESP32-S3-DevKitC-1 |

**不能直接刷的：**

- **ESP32-S3 但只有 2 MB Flash** —— 分区表会越界，需要用 2 MB 分区方案重新编译（改 `PartitionScheme`）。
- **ESP32 / ESP32-C3 / ESP32-C6 / ESP32-H2** —— 指令集不同，须改 FQBN 重新编译，例如 `esp32:esp32:esp32c3`。
- **ESP8266 / 其他厂商芯片** —— 不适用。

固件镜像头部已固定为：`模式=DIO / 速度=80MHz / Flash 容量=4MB`。

---

## 最终产出（烧什么）

| 文件 | 大小 | 烧录地址 | 说明 |
|---|---|---|---|
| **`merged_4mb.bin`** | 4 194 304 B | **`0x0`** | **整片镜像，推荐。一个文件包含引导+分区表+应用** |
| `bootloader.bin` | 19 968 B | `0x0` | 引导程序（分段烧录时用） |
| `partitions.bin` | 3 072 B | `0x8000` | 分区表（分段烧录时用） |
| `app_v43.bin` | 742 032 B | `0x10000` | 应用本体 V4.3（分段烧录时用） |
| `auto_deauth_v43.ino` | 10 064 B | — | 源码 |
| `frame_bypass.c` | 1 055 B | — | 源码依赖（符号覆盖），必须与 .ino 同目录 |

SHA256：

```
E1AFB5CE7A763AFEE476BDDDADA2434CF55E1006EB239E60E2A08452ECD6867C  merged_4mb.bin
66CF6C060FC857C8EA6D6812AAB8E9B27448431846426218B6FB81526B31E564  bootloader.bin
148B959CBFF1C38AA8E1D5C0BA9D612C54997B945E56A63F41223EEF650653A1  partitions.bin
B1E4FE5EAC25BB275431DFB2290E713C96672F7F57B568359802824C3826C06E  app_v43.bin
```

分区表布局（4 MB）：

```
nvs        0x009000    20 KB
otadata    0x00E000     8 KB
app0       0x010000     1.25 MB
app1       0x150000     1.25 MB
spiffs     0x290000     1.375 MB
coredump   0x3F0000    64 KB
```

---

## 烧录步骤

### 方式一：Flash Download Tool（图形界面）

1. 运行 `flash_download_tool_3.9.11.exe`
   选择 **ChipType = ESP32-S3**，**WorkMode = Develop**，**LoadMode = UART**，点击 OK。

2. 界面参数：

   | 项 | 值 |
   |---|---|
   | COM | 板子的串口号 |
   | BAUD | `921600`（不稳定时降到 `460800`） |
   | SPI SPEED | `80MHz` |
   | SPI MODE | `DIO` |
   | **DoNotChgBin** | **必须勾选** |
   | Flash Size | `4MB` |

3. 下载区第一行打勾，文件选 `merged_4mb.bin`，偏移填 `0x0`；其余七行留空。

4. 先点 **ERASE**（整片擦除），完成后点 **START**，出现绿色 `FINISH` 即完成。

5. 按一下 RST，串口 115200 应输出：

   ```
   === 自动断网 V4.3 ===
   ```

> **`DoNotChgBin` 必须勾。** 不勾时工具会按界面参数改写镜像头部的 flash 配置，烧进去会无限重启、串口刷 `invalid header`。
>
> **先 ERASE 再刷。** 若目标板之前跑过其他固件，`otadata`（`0xE000`）可能仍指向 `app1`，此时写入 `app0` 不会被启动，表现为「烧录成功但仍在跑旧程序」。

### 方式二：esptool 命令行

```bash
# 单文件（推荐）
esptool --chip esp32s3 -p COM6 -b 921600 write_flash 0x0 merged_4mb.bin

# 先整片擦除再写
esptool --chip esp32s3 -p COM6 -b 921600 erase_flash
esptool --chip esp32s3 -p COM6 -b 921600 write_flash 0x0 merged_4mb.bin

# 分段写
esptool --chip esp32s3 -p COM6 -b 921600 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 app_v43.bin
```

无自动复位电路的板子，需按住 **BOOT**（GPIO0 拉低）→ 执行命令 → 出现 `Connecting...` 后松开。

### 方式三：Arduino IDE / arduino-cli

```bash
arduino-cli compile -b esp32:esp32:esp32s3 --output-dir ./build ./src
arduino-cli upload  -b esp32:esp32:esp32s3 -p COM6 --input-dir ./build ./src
```

前置条件见 [编译](#编译) 一节 —— **不装 `platform.local.txt` 补丁会链接失败**。

---

## 版本更新

| 版本 | 应用体积 | 变更 |
|---|---|---|
| **V4.3** | 742 032 B | 补上管理帧学习（assoc / auth / deauth / probe resp / action），客户端识别 2~5 个，日志干净。**当前发布版** |
| V4.2 | 742 000 B | 双向学习数据帧，客户端仍只有 0~1 个（环境下行帧太稀疏） |
| V4.1 | 741 904 B | 严格只认 `FromDS=1` 的 AP→STA 帧，过滤过头，客户端数掉到 0 |
| V4.0 | 741 520 B | 首版重构：阻塞式扫描、12-bit 递增序列号、客户端轮次老化、按信道分桶驻留 |
| V3 | 741 376 B | 有 bypass，但 `addr1` 被当作用户端 MAC（实际是 BSSID），客户端表全是 AP 自己的地址 |
| V2 | 675 936 B | 废件，ELF 中不存在 `g_bypass_hits`，白名单未绕过，发不出 deauth 帧 |

### V3 → V4.3 修掉的问题

**P0 — 帧序列号恒定**
V3 里 `fr[22]=0xF0, fr[23]=0xFF` 写死，每一帧的 802.11 序列号完全相同。AP 与网卡会把重复序列号的帧静默丢弃，穿透率极低。
→ V4 起改为 12-bit 递增序列号。

**P0 — 信道驻留时间过短**
V3 用 `int i = (hammer / 5) % apNum` 每 5 帧切一次信道，单信道驻留约 10 ms，抓不住客户端重连窗口。
→ V4 起按键道分桶，每信道驻留 `HAMMER_MS_PER_CH`（当前 140 ms）。

**P0 — 客户端 MAC 学错了对象**
`addr1` 在 `FromDS=1 ToDS=0` 的帧里是**接收端 = STA**，而在 `ToDS=1 FromDS=0` 的帧里是**接收端 = AP 的 BSSID**。V3 无条件取 `addr1`，等于把 AP 自身 MAC 写进客户端表。
→ 修法：读 Frame Control 的 DS 位判方向

```
FromDS=1, ToDS=0 → 客户端 = addr1, AP = addr2   (下行)
ToDS=1, FromDS=0 → 客户端 = addr2, AP = addr1   (上行)
管理帧           → 哪一头命中已知 BSSID，另一端即客户端
```

并附加约束：客户端必须是单播地址、客户端 ≠ BSSID、AP 必须出现在本轮扫描结果内。

**P1 — 客户端表永不老化**
`client_cnt` 只增不减，且漫游后 `client_ap` 不更新。
→ V4 起 `client_seen[]` 记录轮次，超过 `CLIENT_TTL_ROUNDS=6` 未出现即回收。

**P1 — 每个 AP 只踢一个客户端**
→ V4 起踢该 AP 下全部已知客户端。

**P1 — 扫描靠 `delay(500)` 猜完成**
→ V4 起 `esp_wifi_scan_start(&cfg, true)` 阻塞等待，扫描前关 promiscuous、清 AP 列表，扫描后重开。

**P1 — 事件队列溢出**
实机日志反复出现 `E wifi: failed to post WiFi event=1 ret=259` 与 `event=43`（home channel change），内部 32 深事件队列被切信道动作打满。
→ 用 `esp_log_level_set("*", ESP_LOG_NONE)` 静音库日志，并降低切信道频率。

**P2 — 发射缓冲耗尽**
`lasterr=257`（`ESP_ERR_NO_MEM`）大量出现，失败率约 30%。
→ 增加退避重试 3 次（`RETRY_DELAY_US=1500`）、`SEND_GAP_MS` 4 ms，失败率降到约 10%。

**P2 — 循环内 malloc/free**
每轮为 64 条 `wifi_ap_record_t` 分配堆内存。
→ 改用静态缓冲。

**P2 — ISR 中非原子访问**
`client_cnt` / `g_bypass_hits` 在中断回调里读写。
→ 改为 `volatile` 计数，仅在主循环归并。

---

## 工作原理

`esp_wifi_80211_tx()` 在发送前会调用 `ieee80211_raw_frame_sanity_check()`，该函数只放行白名单里的帧类型（beacon、probe request 等），遇到 deauth（Frame Control `0xC0`）直接拒绝，串口打印：

```
wifi: unsupport frame type: 0c0
```

绕过方法分两步：

1. **把库里的符号改成弱符号。** 用二进制编辑把 `libnet80211.a` 中 `ieee80211_raw_frame_sanity_check` 的 ELF 符号绑定从 `GLOBAL` 改为 `WEAK`（只改 `st_info` 一个字节，函数体不动）：

   ```
   0000005c T ieee80211_raw_frame_sanity_check   ← 原厂
   0000005c W ieee80211_raw_frame_sanity_check   ← 改后
   ```

2. **在 sketch 里提供同名强符号**（`frame_bypass.c`）：

   ```c
   volatile long g_bypass_hits = 0;

   int ieee80211_raw_frame_sanity_check(const void *frame, int len, int is_broadcast)
   {
       g_bypass_hits++;
       return 0;   /* 0 = 放行 */
   }
   ```

链接时强符号覆盖弱符号，检查函数变成空实现，deauth 帧即可发出。计数器 `g_bypass_hits` 持续增长即证明绕过生效。

验证（`T` 表示符号来自 `frame_bypass.c`）：

```bash
xtensa-esp32s3-elf-nm build/app.elf | grep raw_frame_sanity_check
# 42002768 T ieee80211_raw_frame_sanity_check
# 3fc9b790 B g_bypass_hits
```

发射侧使用 `WIFI_IF_STA` + promiscuous 模式，`esp_wifi_set_max_tx_power(82)`（20.5 dBm）。帧体 26 字节，`duration` 置 0，Deauth（`0xC0`）与 Disassoc（`0xA0`）、reason code 2/7 交替发送。

---

## 使用边界

- **单射频只能驻留一个信道。** 2.4 GHz 13 个信道轮流驻留，每个信道只有 `HAMMER_MS_PER_CH` 毫秒的窗口，命中率取决于目标重连速度。
- **不支持 5 GHz。** 目标在 5 GHz 频段上时固件完全无效。
- **PMF / 802.11w 会直接丢弃未加密的 deauth。** WPA3 网络、或启用了 PMF 的 WPA2 网络，用这一代手法无效，需要改用 CSA beacon 伪造或 beacon 洪泛。
- **802.11k/v 快速漫游** 会让终端在断开后毫秒级回连。
- **deauth 洪泛是 WIDS 最容易识别的特征之一**，在有无线入侵检测的环境里会立刻暴露。

---

## 编译

环境：Arduino IDE 2.x + **esp32 core 3.3.12**（Arduino IDE 板管理器安装）

### 目录要求

Arduino 要求 sketch 目录名与 `.ino` 文件名一致：

```
auto_deauth/
├─ auto_deauth.ino
└─ frame_bypass.c
```

### 必须的链接补丁

原厂 `libnet80211.a` 里的符号是 `GLOBAL`，直接编会链接失败：

```
multiple definition of 'ieee80211_raw_frame_sanity_check';
frame_bypass.c:20: first defined here
collect2.exe: error: ld returned 1 exit status
```

两种解决办法，任选其一：

**A. 改库符号为 WEAK（推荐，一次搞定）**

找到 `packages/esp32/tools/esp32s3-libs/3.3.12/lib/libnet80211.a`，把目标符号的 `st_info` 从 `GLOBAL` 改成 `WEAK`。改完 `nm` 应显示 `W`。

**B. 加链接参数（不改库）**

新建 `packages/esp32/hardware/esp32/3.3.12/platform.local.txt`，内容一行：

```
compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition
```

该键在 `platform.txt` 中原本为空，覆盖安全。删除该文件即可撤销。

### 板型设置

Board: **ESP32S3 Dev Module**
Flash Size: 4MB（或按实际）
Partition Scheme: Default 4MB with spiffs
PSRAM: Disabled（不使用）
Upload Speed: 921600

编译后 `Sketch uses 741880 bytes (56%)`、`Global variables use 49528 bytes (15%)`。
