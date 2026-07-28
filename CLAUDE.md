# CLAUDE.md — sqc485iv2-mesh-fw-hil(QC 驗收 worktree)

這**不是另一個 repo**。它是 `/Users/delorescelteh/Projects/sqc485iv2-mesh-fw` 的 git worktree,
checkout 在分支 **`test/sq-regression-harness`**。用途只有一個:

> **發版前的 QC 閘門** —— 對一顆真板子把 SQC485Iv2 的**全部功能**逐條跑過,
> 而不是只驗這次改了什麼。

產品本身的知識(產品線、PortNum 256、SQ 協定、TW DTS 參數、brownout 緩解、出廠 PSK)
寫在 **`../sqc485iv2-mesh-fw/CLAUDE.md`**,那份是主文件,**先讀它**。這份只寫測試台的事。

---

## 0. 🚨 這個分支**同時帶著韌體修改**,不是只有測試

這是最容易做出錯誤結論的地方。`test/sq-regression-harness` 除了測試,還含有 main 沒有的
**firmware** commit:

| commit | 內容 | main 上有嗎 |
|---|---|---|
| `497257790` | `fix(modbus)`: 只重試 slave 說值得重試的拒絕 | ❌ |
| `65d08a8d0` | `feat(modbus)`: 把 slave 的拒絕原因帶進 payload | ❌ |
| `68beecc98` | `test(hil)`: 軟體遠端 + 不會把板子丟在半路的閘門 | ❌ |
| `d8acb6741` | `fix(hil)`: serial handshake 快速失敗,不要等 300s | ❌ |

**所以「板上跑什麼韌體」決定了結果。** 如果板子燒的是 main 的 build,
`POLL-07/08/09`(exception 路徑)一定 FAIL —— 那不是回歸,是那顆 image 還沒有這個修正:

```c
/* main 的 modbus.c:一次要求 5 + 2*reg_count bytes。
   Modbus exception 回覆固定只有 5 bytes → 永遠湊不滿 → 被判成 timeout,
   而且整個 retry 預算全燒掉。分支版本改成 staged read + modbus_exception_is_transient()。*/
```

跑 QC 之前**先確認板上的 firmware 就是你要發的那一版**。清單裡的 `ID-04` 就是在做這件事
(比對 `SQ_FW_VERSION` 與 checkout),但它比對的是**版本字串**,擋不住「同版本、不同分支」。
要驗分支韌體就先燒:

```bash
pio run -e sqc485iv2-esp32c3-sx1262 -t upload --upload-port /dev/cu.usbmodem141201
```

---

## 1. 這台 Mac 的測試台(2026-07-28 實測)

```
  SQC485Iv2 ──── USB ────┐
                         ├── 這台 Mac
  USB-RS485 dongle ──────┘
        │
        └── A / B / GND 接到裝置的 RS485 端子
```

| 角色 | 埠 | 備註 |
|---|---|---|
| **DUT** console | `/dev/cu.usbmodem141201` | SER `58:8C:81:B8:A0:3C`,node `!81b8a03c`,env `sqc485iv2-esp32c3-sx1262`(v233 極性) |
| **RS485 dongle** | `/dev/cu.wchusbserial14140` | `1a86:7523` **已接到 DUT 的 A/B/GND,實測可用** ✅ |
| **peer**(`--peer`) | `/dev/cu.usbmodem141301` | SER `50:78:7D:51:BD:B0`,node `!7d51bdb0`,env **`sqc485iv2-v231-esp32c3-sx1262`**(hw v2.3.1),fw `2.7.26.5dc5fdd` / SQ 1.3.2 |
| 鄰居(**不要碰**) | `/dev/cu.usbmodem141101` | SER `58:8C:81:B9:48:00` = SQS-TH-I `!81b94800`。**會深度睡眠**,見 `../sqs-sensor-mesh-fw/CLAUDE.md` §3 |
| 其他 CH340 | `12420` / `12430` | **沒接到任何東西**,拿它們當 `--rs485` 會得到「dongle 聽不到裝置發話」 |

⚠️ **第二顆節點是 `usbmodem141301`,不是 `wchusbserial12430`。** 兩顆 SQC485Iv2 都是
ESP32-C3 native USB(`303a:1001` → `usbmodem*`);`wchusbserial*` 全是 CH340 USB-UART,
不是節點。要分辨哪個是哪個就看 serial number,別靠埠號猜。

⚠️ 三個 CH340 的 VID:PID 一模一樣,`find_port()` 看到多個候選就會拒絕猜測 ——
**一定要明寫 `--rs485 /dev/cu.wchusbserial14140`**。

### peer 節點的兩個注意事項

1. **它跑的是 v231 build、而且韌體比 DUT 舊**(SQ 1.3.2 vs 1.3.3)。當 FakePeer 的載體沒問題
   —— 只需要它是一顆同頻道的 Meshtastic 節點 —— 但**不要**拿它的 QC 結果當 DUT 的結果。
2. **peer 自己的韌體也會回應 `SQ}`**:它看到 `mp.to == self` 就會在**它自己的** RS485 上跑
   rawBridge 並回一個(空的)`SQ{`。所以 FEAT-02 期間會有兩筆回覆競爭 —— FakePeer 那筆帶資料,
   peer 韌體那筆是空的。檢查讀匯流排最多 20s,兩者順序不影響結果,但看 log 時要知道為什麼有兩筆。

```bash
/opt/homebrew/bin/python3 -m venv test/hil/.venv
test/hil/.venv/bin/pip install -r test/hil/requirements.txt
```

---

## 2. 三層測試,不是互相取代

| 層 | 指令 | 需要 | 實測 |
|---|---|---|---|
| host unit | `make -C test/sq_core test` | 無 | ✅ 85 tests / 684 assertions,~1s |
| **QC 驗收** | `test/hil/.venv/bin/python test/hil/sq_hil.py --device … --rs485 …` | 板子 + dongle | **35 條需求,~130s** |
| 唯讀 smoke | `../sqc485iv2-mesh-fw/test/e2e/sqc485iv2_e2e.py` | 只要板子 | 不寫設定,可對現場節點跑 |

- `test/sq_core` —— 把 `firmware_core` 的 C 檔編成 host binary 測,含 committed 的
  golden blob fixture(CI 會檢查 `make_golden.py` 產出跟 checked-in hex 一致)。
  沒有硬體時唯一能保護 wire format 的東西。
- `sq_hil.py` —— **會寫入設定**,`finally` 裡寫回。不要對現場節點跑。
- ⚠️ `pio test -e native` 在 macOS 上編不起來(portduino 的 `Arduino.h` include glibc 專屬的
  `argp.h`),14/14 ERRORED。**不是回歸,不要花時間追。**

---

## 3. 清單怎麼運作:`requirements.py` 是「該測什麼」,不是「測了什麼」

`test/hil/requirements.py` 列出 35 條需求,每條有 id / 標題 / **為什麼重要** / `needs`。
`sq_hil.py` 的每個檢查在斷言前呼叫 `rep.requirement("POLL-04")`,之後的 ok/bad 都歸到那一條。

**這樣做的理由:只回報「跑過什麼」的閘門,沒辦法告訴你它忘了什麼。**
原本的 harness 會斷言節點宣告了全部六個 feature bit(`0x3F`),但其中三個 ——
tunnel、BLE power、deep sleep —— **一個檢查都沒有**。它驗的是「宣告」,不是「功能」。

所以結尾有三種狀態,不能混為一談:

| 狀態 | 意思 |
|---|---|
| `SKIP` | **這張 bench 答不了**(沒有第二顆節點、`--skip-rs485`…),有記錄理由 |
| `NOT RUN` | **沒有人寫這個檢查** → 直接讓整個 run 失敗 |
| `FAIL` | 測了,不通過 |

`NOT RUN` 會 fail 是刻意的:覆蓋率只能**故意**降低(`--allow-gaps`),不能不小心掉。
新增功能時,先在 `requirements.py` 加一條 —— 它會立刻變成 `NOT RUN` 逼你補檢查。

`needs` 的值:`""`(只要 console)/ `rs485` / `reboot` / `peer`(第二顆節點)/ `role`(改 Meshtastic role)。

### 常用指令

```bash
V=test/hil/.venv/bin/python
D="--device /dev/cu.usbmodem141201"
R="--rs485 /dev/cu.wchusbserial14140"

$V test/hil/sq_hil.py $D $R                    # 完整 QC(~130s)
$V test/hil/sq_hil.py $D --skip-rs485          # 只跑 console 那半
$V test/hil/sq_hil.py $D $R --skip-reboot      # 不重開板子
$V test/hil/sq_hil.py $D $R --strict-radio     # 驗工廠映像:電台偏離 = FAIL 不是 warning
$V test/hil/sq_hil.py $D $R --peer /dev/cu.usbmodemXXXX   # 補上 FEAT-02
$V test/hil/sq_hil.py $D $R --deep-sleep       # 補上 FEAT-03(慢、會改 role)
$V test/hil/sq_hil.py $D $R --json qc.json     # 逐條結果 + timing 存成 JSON
```

### 每次跑都會自動留下記錄

**不需要加任何旗標。** 每一次 run 都會在 `test/hil/reports/` 產生一份帶日期時間的記錄,
**內容是繁體中文**(給 QC 人員讀的):

```
qc-20260728-163305-81b8a03c.md     ← 記錄本體(進 git)
qc-20260728-163305-81b8a03c.pdf    ← 同一份的 PDF,方便閱讀 / 傳給人
```

檔名格式 `qc-<YYYYMMDD>-<HHMMSS>-<node id>`,**永遠不覆蓋**。內容除了逐條結果,還帶著
「不在現場的人也看得懂」所需的脈絡:埠位、板子回報的 firmware 與 `SQ_FW_VERSION`、
git 分支/commit、**working tree 有沒有髒**,以及每個沒過的項目附上那句「為什麼重要」。

- **working tree 髒的話報告會明講** —— 否則裡面那個 commit hash 是在騙人。
- `*.pdf` **不進 git**(是同一份資料的另一種呈現,隨時可重算);`.md` 才是記錄本體。
- PDF 算不出來只會印一行提示,**不會**讓 QC run 失敗。

### 中文文案在 `requirements_zh.py`,不在 `requirements.py`

`requirements.py` 是清單的**結構**(id / area / needs),程式與終端機輸出沿用 repo 的英文慣例;
`requirements_zh.py` 只放給人讀的中文。分開是因為兩者變更節奏不同 —— 新增需求要動結構,
潤飾一句說明不該碰程式。

缺翻譯時 `report.py` 會**退回英文原文**(不讓報告產不出來),所以 `tests/test_requirements_zh.py`
會把「漏翻」變成看得見的失敗:少一條、少一個欄位、有孤兒項目、或內容根本沒有中文字,都會 fail。

⚠️ **報告裡的「實測」那一行是英文的,這是刻意的。** 那是 harness 當下量到的原始訊息
(`rep.bad()` 的內容),與終端機輸出逐字相同,翻譯它會讓報告和 log 對不起來。
中文的判讀寫在下一行的「為什麼重要」。

### PDF 是自己畫的,不呼叫外部工具

`report.py` 用 `fpdf2`(已列在 `requirements.txt`)**直接從結果資料畫 PDF**,不先轉 Markdown 再解析。
原本是呼叫 `~/.openclaw/.../md2pdf.py`,換掉的兩個理由:

1. **那是 repo 外的絕對路徑。** 換一台 QC 機器就整個失效 —— 而這個 harness 的用途正是
   在不同機器上當發版閘門。(順帶一提,這台機器上的 `/tmp/md2pdf_venv` 實測是壞的:
   只有 `python`,沒有 `pip` 也沒有 lib。而且 `/tmp` 重開機就沒了。)
2. **它的段落用 fpdf 預設的左右對齊,中文會爆版。** 只要一句中文裡夾一個 ASCII 詞
   (`SQ_FW_VERSION`、`21.9`),justify 就會把那唯一一個空白拉滿整行,實測一行只有兩個詞、
   中間空一大片。所以 `_para()` 一律 `align="L"` —— **改動這個參數會直接毀掉中文排版。**

另外兩個中文排版的坑,動 `report.py` 時要留著:

- **表格必須逐字量測換行**(`cell_lines()`)。中文沒有空白可斷,靠空白斷字的實作會讓長儲存格
  整個衝出欄位。
- **不要讓長段落以 `**` 開頭**。就算之後改回用 md2pdf,它會把那種行當成「粗體單行標題」
  用不換行的 `cell()` 渲染,右邊直接被裁掉 —— 而那正是最需要被讀到的內容。

`--no-report` / `--report-dir DIR` 可以改行為,但**發版跑的那一次不該用**。

---

## 4. 🚨 寫 / 改檢查時會踩的五個坑(全部實測踩過)

### 4.1 板子重開的證據是 `reboot_count`,**不是 `/dev` 節點消失**

最直覺的做法 —— 等 USB CDC 消失再回來 —— **在這顆晶片上永遠不會成立**。
C3 的 USB-JTAG bridge 重新列舉太快,即使 30ms 輪詢也**觀察不到**埠位空窗。
用那個訊號寫出來的 `reboot()` 會把整個 timeout 燒完(實測固定 91.5s),
然後對一顆**根本沒被重開的板子回報成功**。

`myInfo.reboot_count` 每次開機遞增,是正面證據。改用它之後:**91.5s → 4.4s**,而且是真的在驗證。

### 4.2 `SQ?` poll-now **不看** `rs485_enabled`,所以不能拿它測 MODE-01

`handleReceived()` 收到 `SQ?` 直接呼叫 `pollAndSend()`,不檢查 flag —— 這是對的,
操作員明確要求就該照做。`rs485_enabled` 管的是 `runOnce()` 裡的**計時器**。
用 `SQ?` 測會看到「關掉還是有一筆請求」然後誤判成韌體 bug(我第一版就是這樣寫錯的)。
正確做法是設短 interval,**看兩個週期內匯流排有沒有動靜**。

### 4.3 config 推送**不會重排 poll thread**(這就是 CFG-07)

`applyConfigBlob()` 只做 `config_save` + `hal_serial_init`,**沒有碰 OSThread 的排程**。
所以 `runOnce()` 上一輪回傳 `uplink_interval_s * 1000` 之後,節點就睡滿**舊的**間隔。

對測試的影響很大:`quiesce()` 和多數檢查都把 `uplink_interval_s` 設成 3600,
**後面任何量測週期行為的檢查都會被毒到**。要量週期行為就先 `dev.reboot()`
讓計時器從新設定重新起算(現在只要 4.4s,很便宜)。

### 4.4 tunnel 送出的封包**看不到**,所以 FEAT-02 一定要第二顆節點

```c
service->sendToMesh(p, RX_SRC_LOCAL, false);   // forwardTunnel():ccToPhone = false
```

跟 telemetry 不同,轉發出去的 frame **不會**複製給連著的 client —— 它只存在於空中。
單顆節點無論怎麼假裝 peer 都觀測不到。所以沒給 `--peer` 時 FEAT-02 回報 `SKIP` 並附上理由,
而不是降級成一個比較弱的斷言然後宣稱有覆蓋到。

給了 `--peer` 之後實測一次就通,整條鏈都在:DUT 從本地 RS485 讀到 frame →
經 LoRa 轉給 `!7d51bdb0` → FakePeer 回 `SQ{` → DUT 寫回本地匯流排(`010302c0de681c`)。

### 4.5 `SQ>` 的 6-byte link header 漏了會把 UART 卡在 769 baud

`rawBridge()` **純靠長度**判斷有沒有 header(`if (reqlen >= 6)`)。所以最直覺的做法 ——
直接把 8-byte Modbus 框丟給 raw bridge —— 不會被拒絕,而是被重新解讀成
`baud = 0x00000301 = 769`,而且 `hal_serial_init()` 會把 UART **留在**那裡,
之後每一次 poll 也全部 timeout。症狀跟接錯線一模一樣。
一律用 `sq_protocol.bridge_request()` 組請求。

---

## 5. 2026-07-28 的基準線(板上跑 main 的 `2.7.26.0e03c75`)

完整跑法 —— **35 條全部真的執行,沒有 SKIP**:

```bash
V=test/hil/.venv/bin/python
$V test/hil/sq_hil.py --device /dev/cu.usbmodem141201 \
   --rs485 /dev/cu.wchusbserial14140 --peer /dev/cu.usbmodem141301 --deep-sleep
```

```
30 passed, 5 failed, 0 skipped, 0 not run  (35 requirements, 230s)
```

| 需求 | 結論 |
|---|---|
| `CFG-03` 過長 poll plan 被拒絕 | ❌ **真缺陷**。8 polls × 16 regs = 272 bytes > 233,`applyConfigBlob()` 回 status 0,`poll_collect_raw()` 在 poll 邊界靜默丟掉後面的 poll。清單期待 **status 3**(`sq_protocol.CONFIG_STATUS` 已經預留),韌體還沒實作 |
| `CFG-07` 縮短 interval 立即生效 | ❌ **真缺陷**,實測改成 4s 後隔了 **21.9s** 才輪詢。見 §4.3 |
| `POLL-07/08/09` exception 路徑 | ❌ **板上是 main 的 build,沒有那兩個 commit**(§0)。燒分支韌體後應該會過 —— **尚未實際驗證** |
| 其餘 30 條 | ✅ 含 RS485 byte-exact 轉發、fc4、poll_gap、CRC 拒絕、19200 熱切換、unicast + want_ack、blob 拒絕/回溯相容、重開後設定保留、3 次重開全回來、tunnel 端到端、deep sleep 真的睡著並醒來 |

跑完之後 DUT 的 role 回到 `CLIENT`、config blob CRC 仍是 `0x485d`(與整場開始前一致)。

⚠️ **`POLL-07/08/09` 的「燒分支韌體就會過」目前是從原始碼推斷的,不是實測。**
main 的 `modbus.c` 一次要求 `5 + 2*reg_count` bytes,而 exception 回覆固定 5 bytes;
分支的 `modbus.c` 有 `modbus_exception_is_transient()` 與 staged read。要確認就燒了再跑一次。

---

## 6. 這個 harness 不會做的事

- **不碰上游 Meshtastic 的行為**(路由、加密、MQTT、螢幕)。那是上游 CI 的事。
- **不量射頻品質**。`RF-01` 讀的是**執行中的 config**,不是空中的訊號 ——
  它擋的是「出廠設定跑掉」,不是「功率或頻偏對不對」。要量得用頻譜儀。
  (不過 `FEAT-02` 加上 `--peer` 之後,**確實證明了 DUT 真的有在空中收發** ——
  frame 是經由 LoRa 送到另一顆節點再回來的。所以「完全發不出去」這種硬故障擋得住。)
- **不驗 `SQ P` 真的改了 BLE 功率**。那個值寫進獨立的 store key,**沒有讀回介面**;
  `FEAT-01` 能證明的是「被接受、而且沒有污染 config blob」,如實寫在輸出裡。
- **不做冷啟動**(拔電)。`dev.reboot()` 是軟重開。要真的斷電看
  `/Users/delorescelteh/Projects/ppps`。

## 7. 不要在沒有授權下做的事

- **不要對現場 / 客戶節點跑 `sq_hil.py`** —— 它會覆寫設定。隨手檢查用 main 的唯讀 e2e。
- **不要用 `--allow-gaps` 讓 CI 綠燈** —— 那個旗標是給「故意只跑子集」用的,不是給發版用的。
- **不要改 `config.lora` 的 region / tx_power / 頻率**,那是 TW DTS 認證參數。
- **不要把 `SQ_FACTORY_CH1_PSK_HEX` 寫進任何檔案或 commit**。
