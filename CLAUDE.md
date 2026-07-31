# CLAUDE.md — sqc485iv2-mesh-fw

> ⚠️ **retired 2026-07-31 — read [`HANDOVER.md`](HANDOVER.md) FIRST, before anything
> below.** This repo is archived (read-only) on GitHub. The active repo is
> `/Users/delorescelteh/Projects/sqs-sensor-mesh-fw`. Everything past this notice
> describes this repo as it was while alive — still useful for board-level / RS485
> detail that hasn't been superseded, but do not plan new work against it without
> reading the handover first.

這是 **Siliqs 的 Meshtastic fork**(`git@github.com:siliqs/sqc485iv2-mesh-fw.git`),
單一產品線:**SQC485Iv2**(RS485 → Mesh 工業節點)。上游 Meshtastic 的通用知識
(module 架構、Observer、加密、protobuf、CI)都寫在既有文件裡,**這份檔案只寫
「上游文件沒有、而且踩過會痛」的部分**。

## 0. 先讀哪一份

| 檔案 | 內容 | 什麼時候讀 |
|---|---|---|
| `.github/copilot-instructions.md` | **上游主文件**:專案結構、coding conventions、build system、MCP 測試框架 | 任何非瑣碎的修改前 |
| `AGENTS.md` | 主文件的短版指標 + 快速指令表 + house rules | 只要指令速查 |
| **本檔** | SQC485Iv2 特有的事、**這台 Mac 的實測狀態**、測試怎麼跑 | 每次開工前 |
| `variants/esp32c3/sqc485iv2/README.md` | 板級細節(HT-CT62 腳位;注意:電池 ADC 未實作) | 動板級碼時 |
| **`test/hil/CLAUDE.md`** | **QC 驗收測試台**:發版前跑的 35 條需求清單、測試台接線、寫檢查會踩的坑 | 要跑 QC / 動硬體測試時 |
| `test/hil/README.md` | HIL 測試台的接線、`SQ>` 陷阱、匯流排故障診斷 | 要動硬體測試時 |
| `/Users/delorescelteh/Projects/sqs-sensor-mesh-fw/CLAUDE.md` | 姊妹 fork(SQS-TH-I + SQC485 SKU 已搬過去)。**很多共用坑寫在那裡** | 兩邊都要動時 |

house rules 沿用 `AGENTS.md`(不擅自做破壞性裝置操作、一個 serial port 一次只能一個
連線、commit 前 `trunk fmt`、註解最多一兩行、時間節流用 `Throttle` 不要用裸 `millis()`)。

### 語言規則

**本檔用繁體中文,但產出物一律英文** —— code comment、commit message、PR 標題與描述、
log / error 字串、識別字、檔名。這個 repo 的 code、`AGENTS.md`、
`.github/copilot-instructions.md` 都是純英文,不要因為讀了中文文件就跟著飄。
回覆使用者用繁體中文。

文件裡的**路徑、符號、env 變數、指令、commit hash、錯誤訊息原文一律不翻譯**。
那些是拿去 grep 和比對的字串:寫 `SQ_CONFIG_VERSION`、`SQ>`、`sqc485iv2-esp32c3-sx1262`
的原文,不要翻成中文描述。

---

## 1. 這個 fork 是什麼

上游 Meshtastic v2.7.26(`54e0d8d0a`)+ Siliqs SQC485Iv2,ESP32-C3 + SX1262(Heltec HT-CT62 模組)。

| env | 板級 define | 差異 |
|---|---|---|
| `sqc485iv2-esp32c3-sx1262` | `SQC485IV2` | **產品主 env**。DE 極性 = v233(反相) |
| `sqc485iv2-v231-esp32c3-sx1262` | + `SQC485I_DE_INVERTED=0` | v231 硬體:GPIO9 直驅 DE,不經反相器 |
| `sqc485iv2-usbtunnel-esp32c3-sx1262` | + `SQ_USB_TUNNEL` | USB↔USB 透通 SKU,console 導到 NullStream |

⚠️ **`heltec-ht62-esp32c3-sx1262` 不是產品 env。** 它沒有 `-D SQC485IV2`、沒有 TW 電台參數、
沒有 `src/siliqs` include path,build 出來就是一顆跑 LongFast 的普通 Meshtastic 節點。

共用碼在 `src/siliqs/firmware_core/`(與平台無關的 C core:`config.c` 設定 blob、`modbus.c`、
`poll.c`),透過 `include/hal/*.h` 抽象,Meshtastic 這邊的實作是 `hal_meshtastic.cpp`。
`include/config.h` 是**客戶面的設定契約**:`SQ_CONFIG_VERSION`(現為 4)、`SQ_FW_VERSION`
(現為 1.3.3,**每次發版要 bump**)、`SQ_PRODUCT_ID`、feature bits 都在這。

### 空中協定:PortNum **256**(`meshtastic_PortNum_PRIVATE_APP`)

telemetry 與所有 `SQ*` 命令共用同一個 portnum,**靠 payload 內容區分**,因為配置器是把封包
注入本地節點的 API,`mp.from == self`,跟自己的 telemetry loopback 完全一樣:

| payload | 意思 | 回覆 |
|---|---|---|
| `SQ` + ver(2..15) + … | 寫入 config blob | `SQ!` + status(0=applied / 1=invalid / 2=save failed) |
| `SQ?` | poll now | 走 `pollAndSend()` 廣播 + cc 給本地 client |
| `SQV?` | 能力握手 | `SQV` proto, blob_ver, features, fw_len, fw, pid_len, product_id |
| `SQG?` | 讀回目前設定 | `SQG` + config blob |
| `SQP` + int8 | 設 BLE TX power | 無 |
| `SQ>` + **6-byte link header** + frame | USB→RS485 raw bridge | `SQ<` + slave 回覆 |
| `SQ}` + header + frame | RS485↔RS485 tunnel | `SQ{` + slave 回覆 |

`b[2]` 只有落在 2..15 才被當成 config 版本;其他 ASCII 一律是命令/回覆標記,所以自己的回覆
不會被自己吃掉。

**telemetry(raw-forward)格式**:每個 poll 貢獻**固定** `2 + 2*reg_count` bytes
(`[slave][func][data…]`);讀失敗就填**同長度**的錯誤框 `[slave][func|0x80][err…]`,
err code 見 `firmware_core/include/modbus.h`(1=TIMEOUT 2=CRC 3=EXCEPTION 4=SHORT 5=MISMATCH)。
**長度固定是刻意的** —— 掉一個 poll 會讓後面所有 byte 位移,雲端解碼就全錯了。

### git remote / 分支佈局

```
origin     git@github.com:siliqs/sqc485iv2-mesh-fw.git   ← 這個 repo
upstream   https://github.com/meshtastic/firmware.git    ← 上游
```

⚠️ **這個 repo 是舊 fork。** SQC485 SKU 已經搬到 `siliqs/sqs-sensor-mesh-fw`(那邊多了 SQS-TH-I,
並且把這裡的 SQC485 env 整批搬過去)。動任何共用碼之前先確認要改哪一邊,不然兩邊會分岔。

分支:`main` / `develop` / `fix/sqc485iv2-brownout-mitigation` / `fix/sync-sq-fw-version-1.3.1` /
`test/sq-regression-harness`。

**`test/sq-regression-harness` 已於 2026-07-29 合併回 main(fast-forward,`47ba1a0f6`),
整套測試與那批韌體修正現在都在 main 上。** 該分支目前與 main 完全相同,已無作用;
先前為它開的 worktree(`sqc485iv2-mesh-fw-hil`)也已移除 —— 兩份幾乎相同的 checkout
只會讓人(和 agent)在錯的目錄下指令。

將來要對某個 release tag 跑 QC、同時 main 繼續開發,再用 `git worktree add` 開一個,
跑完 `git worktree remove` 收掉,**不要常駐**。

### CI

跟姊妹 fork 相反:**上游 ~28 個 workflow 在這個 repo 全部是 active 的**,PR 會真的跑
`main_matrix` / `trunk_check` / `tests` 等一整排。另外有:

- `release-sqc485iv2.yml` —— tag `sqc485iv2-v*` 觸發,build **產品 env** 並發 Release。
- `sq-regression.yml` —— **已在 main 上生效**(2026-07-29 首次執行並通過,677 秒)。
  觸發:`push` 到 main、任何 `pull_request`、`workflow_dispatch`。兩個 job:
  `firmware_core host tests`(含 golden fixture 一致性)與 `firmware build + certified defaults`。

---

## 2. 這台 Mac 的實測狀態(2026-07-28 驗證)

| 項目 | 狀態 |
|---|---|
| `pio` | `/opt/homebrew/bin/pio` ✅ 增量 build `sqc485iv2-esp32c3-sx1262` **13.5 秒** SUCCESS |
| PlatformIO Core | ⚠️ 裝了兩份(6.1.18 / 6.1.19),每次都印 "Obsolete PIO Core" 警告。功能正常 |
| `meshtastic` Python lib | 可從 `/opt/homebrew/bin/python3` 直接 `import` ✅ 寫 e2e 腳本不必開 venv |
| `pyserial` | 3.5 ✅ |
| **`pio test -e native`** | ❌ **在 macOS 上完全編不起來**,14/14 ERRORED。見下 |
| `pkg-config` | ❌ 沒裝(native build 會抱怨,但不是主因) |
| `gtimeout` / `timeout` | ❌ 沒有。不要在指令裡用 `timeout` |
| DUT | `/dev/cu.usbmodem141201`(303a:1001,SER `58:8C:81:B8:A0:3C`,node `!81b8a03c`)✅ |
| 第二顆節點 | `/dev/cu.usbmodem141301`(SER `50:78:7D:51:BD:B0`,node `!7d51bdb0`)—— **env `sqc485iv2-v231-esp32c3-sx1262`**,hw v2.3.1,韌體較舊(SQ 1.3.2) |
| RS485 dongle | `/dev/cu.wchusbserial14140`(1a86:7523)已接到 DUT 的 A/B ✅ |

### 🚨 `pio test -e native` 在 macOS 上是死路,不要浪費時間

```
framework-portduino/cores/portduino/Arduino.h:9:10: fatal error: 'argp.h' file not found
```

`argp.h` 是 glibc 專屬,macOS 沒有。這是 **portduino 的平台限制,不是專案回歸** ——
14 個 test 全部在 build 階段就掛掉,一個都沒跑到。要跑 host 測試請用 §4 的 `test/sq_core`
(那一套是自己的 Makefile + 手寫 runner,不碰 portduino,1 秒跑完 85 個 test)。

---

## 3. 🚨 這顆節點**不會**深度睡眠 —— 跟 SQS-TH-I 完全相反

姊妹 fork 的 CLAUDE.md §3 花了很大篇幅講「USB CDC 一分鐘只出現幾秒」。
**在 SQC485Iv2 上不要套那套心智模型。** `sleepMode()` 的條件是三個 AND:

```c
g_cfg.power.deep_sleep && g_cfg.rs485_enabled &&
config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE
```

實測這顆 DUT:`deep_sleep=true`、`rs485_enabled=true`,但 **role = `CLIENT`**(不是 `CLIENT_MUTE`)
→ `sleepMode()` 回 0 → 永遠不睡。所以:

- `/dev/cu.usbmodem141201` **常駐**,不會消失,不需要搶窗口、不需要 PPPS 冷啟動。
- `pio device monitor` / `meshtastic --port` 可以正常當第一步用。

要重現睡眠行為,得先把 role 改成 `CLIENT_MUTE`。改了之後 §3 那一整套(搶窗口、
`SQ_CONNECT_GRACE_MS` 30 秒寬限、PPPS 斷電)才適用。

另外 `clientConnected()` 看的是 `service->api_state`(API client 已連線),
**不是** SQS-TH-I 那個 `Serial` 的 HWCDC `operator bool()`。行為相近但判斷來源不同。

---

## 4. 測試怎麼跑(三層,分屬不同分支)

| 層 | 在哪 | 指令 | 需要什麼 | 實測 |
|---|---|---|---|---|
| host unit | `test/sq_core/` | `make -C test/sq_core test` | 無 | ✅ **85 tests / 684 assertions PASS**,~1s |
| **QC 驗收** | `test/hil/` | 見 `test/hil/CLAUDE.md` | 板子 + RS485 dongle(+ 第二顆節點) | **35 條需求**:34 pass / 1 fail / **0 skip / 0 not run**,~211s |
| e2e smoke | `test/e2e/sqc485iv2_e2e.py` | `./test/e2e/sqc485iv2_e2e.py --port /dev/cu.usbmodem141201` | 只要板子 | ✅ **26 passed / 0 failed / 2 skipped** |

**QC 驗收是發版前的閘門** —— 逐條跑完產品的全部功能,`test/hil/requirements.py` 是清單本身,
沒有人寫檢查的需求會顯示 `NOT RUN` 並讓整個 run 失敗。細節見 `test/hil/CLAUDE.md`。

⚠️ **跑之前先確認板上燒的就是你要發的那一版。** `SQ_FW_VERSION` 字串**不足以**識別 build ——
main 與 harness 分支兩邊都曾是 `1.3.3`,韌體卻差了 613 行。能識別的是裝置回報的
`firmware_version` 裡的 git hash。`test/e2e` 與 `ID-04` 都已改成比對**韌體相關路徑的 tree 物件**,
所以純文件的 commit 不會誤報。

每一次 QC run 都會自動在 `test/hil/reports/` 留下一份**繁體中文**的記錄
(`qc-<YYYYMMDD>-<HHMMSS>-<node id>.md` + 同名 PDF,永不覆蓋),內含逐條結果、
板子回報的 firmware、git commit、以及 working tree 是否乾淨 —— 發版時要留存的就是那份 `.md`。

三者的定位不同,不是互相取代:

- **`test/sq_core`** —— 把 `firmware_core` 的 C 檔直接編成 host binary 測,含 golden blob
  fixture(`golden/*.hex` 是 committed output,CI 會檢查 `make_golden.py` 產出跟它一致)。
  這是唯一能在沒有硬體時保護 wire format 的東西。
- **`test/hil/sq_hil.py`** —— 完整驗收。**會寫入 probe config,再在 `finally` 裡寫回原設定。**
  適合出貨前 / 改過協定後跑,不適合對現場節點隨手跑。`--strict-radio` 用在驗收工廠映像。
- **`test/e2e/sqc485iv2_e2e.py`** —— **唯讀** smoke test,不寫任何設定、不需要 venv、不需要
  RS485 治具。適合隨手確認一顆節點是好的,或確認板上跑的就是手上這棵樹。它多做兩件 HIL 沒做的事:
  比對 `firmware_version` 與 `git rev-parse HEAD`,以及用 `SQG?` 讀回來的**活的** poll plan
  去切 `SQ?` 的 raw-forward payload(驗證長度與對齊)。

### 建 HIL 環境

```bash
/opt/homebrew/bin/python3 -m venv test/hil/.venv
test/hil/.venv/bin/pip install -r test/hil/requirements.txt
./bin/sq-test.sh --hil -- --device /dev/cu.usbmodem141201 --skip-rs485
```

### 🚨 寫 e2e 腳本必踩的兩個坑

**(1) 連上去的瞬間會被灌一批歷史封包。** `SerialInterface` 建構完成時會把節點暫存的封包
一次 replay 出來(實測 t=0.1s 收到十幾筆,包含好幾筆舊的 raw-forward)。
**沒有做 watermark 的測試會把舊封包當成新回覆**,得到假的 PASS。送出請求前先記下
`len(inbox)`,只認之後才進來的。

**(2) 任何要上 RS485 匯流排的請求都要等 ~25 秒,不是帳面上的 12 秒。** 預設 plan 是
3 polls × (3 retries + 1) × 1000ms timeout = 12s,但請求還得**排在週期 poller 後面**
(`runOnce()` 跟 `handleReceived()` 都在同一條 module thread 上)。`SQ?` 實測 24.7s;
`SQ>` raw bridge 本身只要 1s,但同樣會被排隊,給 15s 就會間歇性逾時 —— 實測第三輪就中。
兩者都給 **40s** 才穩(`bus_budget()` 的算法:`10 + 2.5 × plan`)。

### 🚨 `SQ>` 的 6-byte link header 是**強制**的,漏了會把 UART 卡在 769 baud

`rawBridge()` **純靠長度**判斷有沒有 header:`if (reqlen >= 6)`。所以最直覺的做法 ——
直接把 8-byte Modbus 讀取框丟給 raw bridge —— 不會被拒絕,而是被**重新解讀**:

```
01 03 00 00 00 02 C4 0B   →   baud = 0x00000301 = 769, parity 0, stop 2, frame = C4 0B
```

而且 `hal_serial_init()` 會把 UART **留在** 769 baud,之後每一次週期 poll 也全部 timeout。
症狀是整條匯流排啞掉,看起來跟接錯線一模一樣。正確格式:

```
'S','Q','>' | baud(u32 LE) | parity(u8) | stop_bits(u8) | frame…
```

---

## 5. QC 驗收找到的韌體缺陷(2026-07-28 實跑,板上是 main 的 `2.7.26.0e03c75`)

**(1) 過長的 poll plan 被靜默接受(CFG-03)。** 8 個 poll 需要 272 bytes,但一個封包只裝得下 233。
`applyConfigBlob()` 回 status 0(applied),而 `poll_collect_raw()` 在 poll 邊界
`break` 把後面的 poll 丟掉 —— 使用者不會收到任何警告,只會發現有些點位永遠沒資料。
清單期待 **status 3**(`sq_protocol.CONFIG_STATUS` 已經預留這個碼),韌體還沒實作。

**(1b) 縮短回報間隔不會立即生效(CFG-07)。** `applyConfigBlob()` 只做 `config_save` +
`hal_serial_init`,**沒有重排 OSThread 的排程**。所以 `runOnce()` 上一輪回傳
`uplink_interval_s * 1000` 之後,節點會睡滿**舊的**間隔才理會新設定。
實測:從 25s 改成 4s,下一次輪詢隔了 **21.9s**。把現場單位從每小時改成每分鐘回報,
它會有長達一小時看起來像死掉。

**(1c) Modbus exception 被當成 timeout(POLL-07/08/09)。** main 的 `modbus.c` 一次要求
`5 + 2*reg_count` bytes,而 exception 回覆固定只有 5 bytes → 永遠湊不滿 → 判成 timeout,
而且**燒掉整個 retry 預算**(實測 8.1s)。技術人員會把「poll plan 寫錯暫存器」看成「線沒接」。
⚠️ **這個已經在 `test/sq-regression-harness` 分支修好了**(`497257790` + `65d08a8d0`,
改成 staged read),只是還沒進 main。

**(2) RS485 匯流排 —— 已解決。** 一開始兩個 CH340 埠(`12420` / `12430`)都聽不到裝置發話,
原因是 **A/B/GND 沒接線**。使用者後來加了 `/dev/cu.wchusbserial14140` 並接好線,
迴路測試與 Modbus e2e 隨即全過(payload byte-exact)。
⚠️ 三個 CH340 的 VID:PID 一樣,`find_port()` 會拒絕猜測 —— **一定要明寫 `--rs485`**。

`test/e2e` 那支唯讀 smoke test 仍有兩項 `skip`(「有 Modbus slave 回話」、「raw bridge 收到
slave bytes」),因為它**不會自己起一個 Modbus slave**(唯讀、不需治具是它的定位)。
要驗 RS485 請用 QC worktree 的 `sq_hil.py`,它會自己用 dongle 扮演 slave。

---

## 6. 電台預設值:兩個地方要同步,而且是法規參數

TW DTS 認證的 radio profile(922.5 MHz / BW500 / SF9 / CR4:5 / region TW / **22 dBm**)寫在**兩處**,
都用 `#ifdef SQC485IV2` 包住:

- `src/mesh/Channels.cpp` → `initDefaultLoraConfig()` ← **權威來源**(在 NodeDB 之後跑,會覆蓋它)
- `src/mesh/NodeDB.cpp` → `installDefaultConfig()`

`tx_power` 必須釘在 22 dBm,**不可以留 0(auto)**:留 0 的話 `RadioInterface` 會把區域上限
(TW = 27 dBm)寫回 config,而 SX1262 的 22 dBm 上限是在晶片驅動裡才套用 —— 裝置會**回報 27
但實際發 22**,型式認證時看設定會以為是 500 mW。commit `d33fc7b46` 就是在修這件事。

`NodeDB.cpp` 另外還有一個 `#ifdef SQC485IV2`:**出廠預設關閉 Bluetooth**
(BLE 仍然編譯進去,是 runtime default,可從配置器打開)。

實測這顆 DUT 七項全中:`use_preset=False BW500 SF9 CR5 922.5MHz TW 22dBm`,BLE off。

---

## 7. Boot brownout 緩解 —— 現在可以說「已在硬體上驗證」了

commit `64b4a3eb9` 的訊息寫著 *"Not verified on hardware yet"*。**2026-07-28 補上驗證:**
DUT 跑的就是 `2.7.26.0e03c75`(= HEAD `0e03c7560`),`reboot_count = 9`,整場測試(build /
HIL / e2e,含多次 config 寫入與 RF 發射)都沒有 boot loop。

這塊板子 3.3V 沒有 bulk decoupling,RF 上電的電流階躍會把電軌壓到 C3 的 brownout 門檻
(~2.51V)以下。韌體修不了 >0.8V 的下陷,只能削同時發生的基載:

- `powerHAL_init()` 之後立刻 `setCpuFrequencyMhz(80)`
- boot LED **延後到 radio 起來之後**才點亮(它也是被卸掉的負載之一)
- `initLoRa()` 前 `delay(200)` 讓電軌穩定,之後才回 160 MHz 並點燈

**硬體沒有改(電容一直沒加上去),所以這段是 load-bearing,不是死碼。** 動 `src/main.cpp`
的 setup 順序時要留意別把它拆散。

---

## 8. 出廠頻道 PSK 從來不在 repo 裡

`bin/platformio-custom.py` 讀兩個環境變數:

```bash
SQ_FACTORY_CH1_NAME=<頻道名> SQ_FACTORY_CH1_PSK_HEX=<hex,無分隔> \
  pio run -e sqc485iv2-esp32c3-sx1262
```

有設才會生出 `$BUILD_DIR/sq_factory_prefs.h` 並 `-include` 進去;沒設就是 no-op。
(PSK 陣列必須走 header,`-D` 帶大括號會被 shell brace expansion 弄壞。)

**因此 CI 產出的 artifact 不是可出貨的映像** —— `release-sqc485iv2.yml` 自己註明了這件事。
要出貨的 bin 得在本機設好 env 再 build,PSK 從 1Password 取(見下)。

### 🔑 出廠 PSK:單一來源,而且是**公開值**(2026-07-29 起)

**一律用這一筆,不要自己產、不要向使用者要、不要沿用任何舊值:**

```
op://project-bot/siliqs-factory-ch1-psk/psk_hex        ← 32 bytes (AES256)
op://project-bot/siliqs-factory-ch1-psk/channel_name   ← "siliqs"
```

**全機種共用一把**(不分產品線、不分客戶)。老闆的決定:出廠給一把固定的,
**客戶佈署前自行更換**。

#### ⚠️ 這把 PSK **不是機密**,不要把它當機密處理

老闆 2026-07-29 明確決定:**出廠映像會公開發布**(方便測試、也讓使用者可以自行驗證韌體)。
所以這個值等同公開 —— 它同時存在於每一顆出貨板子的 flash、以及公開下載的 binary 裡。

實務上的意思:

- **可以**把它放進公開的 build、release 資產、CI 變數、對外文件
- **不要**在文件或程式裡宣稱它是秘密 —— 那會讓後面的人對安全性做出錯誤假設
- 1Password 那筆的角色是**單一事實來源**(避免有人手打錯或用到舊值),**不是保險箱**
- 對外說明產品時,頻道安全性要講的是「客戶佈署前必須更換」,不能講「出廠即加密」

`RF-03` 仍然有效,但它的意義要講準:它比對的是 **Meshtastic 官方公開預設金鑰**,
所以它擋的是「CI 的 binary 被當成品燒」,**不是**「金鑰是私有的」。

#### 兩點推論,不要搞錯

- **輪替這筆 1Password 項目只影響之後建置的韌體,現場的板子完全不受影響。** 輪替不是補救手段。
- 頻道名不帶客戶名(舊的 `nafco-…` 是客戶別命名,已停用)。

出貨映像的建法:

```bash
OP=/Users/delorescelteh/Projects/19_how_to_use_1password/scripts/op
SQ_FACTORY_CH1_NAME="$("$OP" read 'op://project-bot/siliqs-factory-ch1-psk/channel_name')" \
SQ_FACTORY_CH1_PSK_HEX="$("$OP" read 'op://project-bot/siliqs-factory-ch1-psk/psk_hex')" \
  pio run -e sqc485iv2-esp32c3-sx1262
```

⚠️ **`mac-bot` vault 裡那筆舊的 `nafco-meshtastic-channel` 是 17 bytes —— 非法的 AES 長度,
不要拿它 build。** 保留它是因為沒人知道那 17 bytes 原本是什麼,現場可能有板子在用。

⚠️ **`op` 要從 VS Code / SSH 這一側跑。** 實測:Background launchd session(SSH / VS Code remote)
走 `~/.config/op/sa-token` 正常,4 秒回應;**Aqua(GUI)session 反而會卡死** ——
`op` 會去走 1Password 桌面 app 整合,停在一個沒人按的授權視窗上(遠端時永遠不會被按)。
這跟 `op` skill 文件寫的方向相反,以實測為準。

**2026-07-30 補充:Aqua session 卡死的真正根因是 Full Disk Access,不是「一定要走 Background
session」。** 在 Mac-mini-m4 上實測:跑 gateway 的 node process(`launchd` 直接啟動,不經
Terminal.app)從未被授權 Full Disk Access,導致**任何**碰 `~/Library/Group Containers/
2BUA8C4S2C.com.1password/` 的呼叫(`op`、甚至一個單純的 `find`)都無限卡住(0% CPU,不報錯、
不 timeout)——連 `tmutil latestbackup` 都會用一模一樣的方式卡住/報錯,這是同一個 TCC 缺口,
不是 1Password 專屬的怪癖。系統設定 → 隱私權與安全性 → 完整磁碟取用權限,把 gateway 實際跑
的那個 node 二進位檔(`/opt/homebrew/opt/node@24/bin/node`,用 `launchctl list | grep openclaw`
+ `PlistBuddy -c "Print :ProgramArguments"` 對應的 plist 找出正確路徑)加進去、重啟 gateway,
Aqua session 下 `op whoami` 就能在 2 秒內正常回應。**這條路徑本來就沒試過** ——上面那段
「一定要走 Background session」的結論是在沒檢查 FDA 的情況下下的,兩者不衝突:FDA 沒開,
Background session(走檔案 fallback)能繞過去;FDA 開了,Aqua session 也能直接用桌面整合。
遇到同樣的卡死,先查 `tmutil latestbackup` 有沒有報 FDA 錯誤,那是比重試 `op` 快得多的診斷。

### 🚨 出廠頻道**只對全新的板子生效** —— 重燒一顆用過的板子不會套用

`USERPREFS_CHANNEL_1_*` 只在 `Channels::initDefaults()` 裡被讀,而它的唯一呼叫點是:

```cpp
// NodeDB.cpp:497  resetRadioConfig()
if (channelFile.channels_count != MAX_NUM_CHANNELS) {
    channels.initDefaults();
}
```

**頻道檔一旦存在且完整(`channels_count == 8`),這段就永遠不會跑。**
而燒錄 app 分區**不會**動到 LittleFS(`test/hil/CLAUDE.md` §0 實測),所以:

- **產線上的新板子** —— NodeDB 是空的 → 出廠頻道正常寫入 ✅
- **重燒一顆已經用過 / 客戶退回 / 開發用的板子** —— 頻道檔還在 →
  **出廠頻道靜默地不會被套用**,板子留在原本的頻道上,而且**沒有任何錯誤訊息** ❌

2026-07-29 在 DUT `!81b8a03c` 上實測:`channels_count = 8`、`channel[0] psk_len=1`
(公開預設)、`channel[1] psk_len=0`(未啟用)。燒出貨映像上去,這些**不會改變**。

要讓出廠頻道生效,得先把頻道檔清掉(factory reset 或 `esptool erase_flash`)——
那是不可逆操作,**要先問過**。

**這條對出貨的意義**:任何「重工 / 維修後重燒」的流程,如果只燒 app 分區,
出貨的板子會帶著上一手的頻道設定離開工廠。`RF-03` 抓得到,前提是有人跑它。

**`RF-03` 就是擋這件事的閘門**(`needs="factory"`,平常 SKIP):

```bash
$V test/hil/sq_hil.py --device … --rs485 … --factory --strict-radio
```

它檢查 channel[1] 存在、有名字、PSK 長度是 16 或 32、而且不是 Meshtastic 公開預設。
報告裡只印長度與判定,**不會印出 PSK 的值**。

---

## 9. 常用指令

```bash
# build(產品 env,不是 heltec-ht62)
pio run -e sqc485iv2-esp32c3-sx1262

# 燒錄(這顆節點不會睡,不需要搶窗口;upload_speed 釘在 115200,~2.1MB 約 3 分鐘)
pio run -e sqc485iv2-esp32c3-sx1262 -t upload --upload-port /dev/cu.usbmodem141201

# 唯讀 smoke test(main,不寫設定、不需要治具)
./test/e2e/sqc485iv2_e2e.py --port /dev/cu.usbmodem141201

# host unit tests(~1 秒)
make -C test/sq_core test

# 完整 QC 驗收(會寫設定再寫回)
./bin/sq-test.sh --hil -- --device /dev/cu.usbmodem141201 \
  --rs485 /dev/cu.wchusbserial14140 --peer /dev/cu.usbmodem141301 --deep-sleep

# ❌ 不要跑:pio test -e native  —— macOS 上 portduino 編不過(§2)

# 送 review 之前
trunk fmt
```

## 10. 不要在沒有授權下做的事

沿用 `AGENTS.md` 的清單,這個 fork 額外加四條:

- **不要改 `config.lora` 的 region / tx_power / 頻率**,那是 TW DTS 認證參數(§6)
- ~~不要把 `SQ_FACTORY_CH1_PSK_HEX` 寫進任何檔案或 commit~~ ——
  **這條已作廢(2026-07-29)。出廠 PSK 是公開值,見 §8。不要再把它當機密處理,也不要再問。**
- **不要對現場節點跑 `sq_hil.py`** —— 它會覆寫設定(雖然會寫回)。要隨手檢查用 §4 的唯讀 e2e
- **不要用 `heltec-ht62-esp32c3-sx1262` env 燒產品板** —— 會變成一顆跑 LongFast 的普通節點(§1)
