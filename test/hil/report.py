"""
report.py — 把每一次 QC 驗收記錄成一份帶日期時間的檔案(Markdown + PDF)。

只印在終端機上的閘門,回答不了 QC 真正會被問的那個問題:**出貨那天,這塊板子到底
做了什麼?** 所以每一次執行都會留下一份記錄 —— 一次一個檔,檔名帶著執行的時刻與節點
編號,而且永不覆蓋。

報告內容是「逐條結果」加上「判讀它所需要的脈絡」:哪些埠、哪一版韌體、哪一個 git
commit —— 以及 working tree 乾不乾淨,否則報告裡那個 commit hash 是在騙人。沒通過的
項目會附上 `requirements_zh.py` 裡那句「為什麼重要」,讓不在現場的人也讀得懂。

## 為什麼自己畫 PDF,而不是丟給外部的 md2pdf

本來是呼叫 `~/.openclaw/.../md2pdf.py`,有兩個問題:

1. **那是 repo 外的絕對路徑。** 換一台 QC 機器就整個失效,而這個 harness 的用途正是
   在不同機器上當發版閘門。
2. **它的段落用 fpdf 預設的左右對齊(justify),中文會爆版。** 只要一句中文裡夾一個
   ASCII 詞(`SQ_FW_VERSION`、`21.9`),justify 就會把那唯一一個空白拉滿整行 —— 實測
   一行只有兩個詞、中間空一大片。

既然報告的結構是這個檔自己產生的,就沒有理由先轉成 Markdown 再解析回來。PDF 直接從
同一份資料畫,版面完全可控,相依只有 `fpdf2`(已列在 requirements.txt)。
Markdown 仍然照產,那才是進 git 的記錄本體。
"""

from __future__ import annotations

import datetime as _dt
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import requirements as req  # noqa: E402
import requirements_zh as zh  # noqa: E402

VERDICT_ZH = {
    req.PASS: "通過",
    req.FAIL: "失敗",
    req.SKIP: "略過",
    req.NOT_RUN: "未執行",
}

# 系統內建的 CJK 字型;缺字會讓整份報告變成一堆方框,所以找不到就直接說。
CJK_FONTS = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
]

INK = (38, 50, 62)
MUTED = (110, 120, 130)
RULE = (205, 212, 219)
BAND = (245, 247, 249)
RED = (176, 42, 42)
GREEN = (26, 122, 68)
AMBER = (166, 110, 12)

STATE_COLOUR = {
    req.PASS: GREEN,
    req.FAIL: RED,
    req.SKIP: AMBER,
    req.NOT_RUN: RED,
}


def text_for(req_id: str) -> tuple[str, str]:
    """中文標題與理由;缺翻譯就退回英文原文,不讓報告產不出來。"""
    entry = zh.TEXT.get(req_id, {})
    source = req.BY_ID[req_id]
    return entry.get("title") or source.title, entry.get("why") or source.why


# ── git 出處 ─────────────────────────────────────────────────────────────────


def _git(repo: pathlib.Path, *args: str) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), *args], capture_output=True, text=True, timeout=10
        )
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:  # noqa: BLE001 — 出處是加分項,不是必要條件
        return ""


def git_context(repo: pathlib.Path) -> dict:
    dirty = _git(repo, "status", "--porcelain")
    return {
        "branch": _git(repo, "rev-parse", "--abbrev-ref", "HEAD") or "(未知)",
        "commit": _git(repo, "rev-parse", "--short=9", "HEAD") or "(未知)",
        "subject": _git(repo, "log", "-1", "--format=%s"),
        "dirty": bool(dirty),
        "dirty_count": len(dirty.splitlines()) if dirty else 0,
    }


# ── 共用的資料整理 ───────────────────────────────────────────────────────────


def _summary(rep) -> dict:
    tally = {
        state: sum(1 for v in rep.status.values() if v == state)
        for state in (req.PASS, req.FAIL, req.SKIP, req.NOT_RUN)
    }
    gaps = tally[req.NOT_RUN]
    passed = tally[req.FAIL] == 0 and gaps == 0
    if passed:
        headline = f"全部 {len(req.REQUIREMENTS)} 條需求都已實際執行,無任何失敗。"
    else:
        parts = []
        if tally[req.FAIL]:
            parts.append(f"{tally[req.FAIL]} 條失敗")
        if gaps:
            parts.append(f"{gaps} 條從未被執行")
        headline = (
            f"{len(req.REQUIREMENTS)} 條需求中,{'、'.join(parts)}。此 build 不可放行出貨。"
        )
    return {"tally": tally, "gaps": gaps, "passed": passed, "headline": headline}


def _dut_rows(meta: dict) -> list[list[str]]:
    dut = meta.get("dut", {})
    return [
        ["節點", dut.get("node", "—")],
        ["產品", dut.get("product_id", "—")],
        ["韌體", dut.get("firmware", "—")],
        ["SQ_FW_VERSION", dut.get("sq_fw", "—")],
        ["Build env", dut.get("pio_env", "—")],
        ["設定 blob 版本", str(dut.get("blob_version", "—"))],
        ["功能位元", dut.get("features", "—")],
        ["開機次數", str(dut.get("reboot_count", "—"))],
    ]


def _bench_rows(meta: dict) -> list[list[str]]:
    return [
        ["DUT console", meta.get("device_port", "—")],
        ["RS485 dongle", meta.get("rs485_port") or "未使用"],
        ["對端節點", meta.get("peer_port") or "未使用"],
    ]


def _source_rows(meta: dict) -> list[list[str]]:
    git = meta.get("git", {})
    return [
        ["分支", git.get("branch", "—")],
        ["Commit", f"{git.get('commit', '—')} {git.get('subject', '')}".strip()],
        [
            "Working tree",
            "乾淨"
            if not git.get("dirty")
            else f"已修改({git.get('dirty_count')} 個檔案與該 commit 不同)",
        ],
    ]


SKIP_NOTE = (
    "「略過」表示這張測試台無法回答該需求(理由列在下方)。"
    "「未執行」表示根本沒有對應的檢查 —— 它本身就會讓閘門失敗,"
    "因為一條沒有人測過的需求,比一條測過但失敗的需求更糟。"
)

OBSERVED_NOTE = (
    "「實測」是 harness 當下量到的原始訊息,與終端機輸出逐字相同,故保留原文;"
    "「為什麼重要」是這條需求的中文說明。"
)


# ── Markdown(進 git 的記錄本體)────────────────────────────────────────────


def _table_md(header: list[str], rows: list[list[str]]) -> list[str]:
    if not rows:
        return []
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return out


def build_markdown(rep, meta: dict) -> str:
    s = _summary(rep)
    L: list[str] = []
    L.append(f"# SQC485Iv2 QC 驗收報告 — {'通過' if s['passed'] else '未通過'}")
    L.append("")
    L.append(f"**{meta['started_human']}**")
    L.append("")
    L.append(s["headline"])
    L.append("")

    L.append("## 結果")
    L.append("")
    L += _table_md(
        ["判定", "通過", "失敗", "略過", "未執行", "耗時"],
        [[
            "通過" if s["passed"] else "未通過",
            s["tally"][req.PASS],
            s["tally"][req.FAIL],
            s["tally"][req.SKIP],
            s["gaps"],
            f"{meta['elapsed_s']:.0f} 秒",
        ]],
    )
    L.append("")
    L.append(SKIP_NOTE)
    L.append("")

    L.append("## 受測裝置")
    L.append("")
    L += _table_md(["", ""], _dut_rows(meta))
    L.append("")

    L.append("## 測試台")
    L.append("")
    L += _table_md(["角色", "埠"], _bench_rows(meta))
    L.append("")
    L.append(f"執行指令:`{meta.get('command', '')}`")
    L.append("")

    L.append("## 受測原始碼")
    L.append("")
    L += _table_md(["", ""], _source_rows(meta))
    if meta.get("git", {}).get("dirty"):
        L.append("")
        L.append(
            "> 這次執行時 working tree 有未提交的修改,"
            "所以上面那個 commit 並不足以完整識別被測試的內容。"
        )
    L.append("")

    L.append("## 驗收清單")
    L.append("")
    area = None
    for r in req.REQUIREMENTS:
        if r.area != area:
            area = r.area
            L.append("")
            L.append(f"### {zh.AREAS.get(area, area)}")
            L.append("")
            L.append("| 判定 | 編號 | 需求 |")
            L.append("|---|---|---|")
        title, _ = text_for(r.id)
        L.append(f"| {VERDICT_ZH[rep.status[r.id]]} | {r.id} | {title} |")
    L.append("")

    bad = [r for r in req.REQUIREMENTS if rep.status[r.id] == req.FAIL]
    if bad:
        L.append("## 未通過項目")
        L.append("")
        L.append(OBSERVED_NOTE)
        L.append("")
        for r in bad:
            title, why = text_for(r.id)
            L.append(f"### {r.id} — {title}")
            L.append("")
            note = rep.notes.get(r.id)
            if note:
                L.append(f"實測:{note}")
                L.append("")
            L.append(f"為什麼重要:{why}。")
            L.append("")

    unrun = [r for r in req.REQUIREMENTS if rep.status[r.id] == req.NOT_RUN]
    if unrun:
        L.append("## 未執行項目")
        L.append("")
        L.append(
            "沒有任何檢查認領這些需求。請補上檢查,或明確記錄一次豁免 —— "
            "不要用 `--allow-gaps` 把閘門洗成綠燈。"
        )
        L.append("")
        L += _table_md(
            ["編號", "需求"], [[r.id, text_for(r.id)[0]] for r in unrun]
        )
        L.append("")

    skipped = [r for r in req.REQUIREMENTS if rep.status[r.id] == req.SKIP]
    if skipped:
        L.append("## 略過項目")
        L.append("")
        L += _table_md(
            ["編號", "需求", "原因"],
            [[r.id, text_for(r.id)[0], rep.notes.get(r.id, "—")] for r in skipped],
        )
        L.append("")

    if rep.timings:
        L.append("## 量測時間")
        L.append("")
        L += _table_md(
            ["量測項目", "數值"],
            [[k, f"{v * 1000:.0f} ms"] for k, v in rep.timings.items()],
        )
        L.append("")

    L.append("---")
    L.append("")
    L.append(
        f"由 `test/hil/sq_hil.py` 於 {meta['started_human']} 產生。"
        "清單本身是 `test/hil/requirements.py`,中文文案在 `test/hil/requirements_zh.py`。"
    )
    L.append("")
    return "\n".join(L)


# ── PDF(自帶排版,不依賴外部工具)──────────────────────────────────────────


def _find_font() -> str | None:
    for path in CJK_FONTS:
        if pathlib.Path(path).exists():
            return path
    return None


def _new_pdf(font_path: str):
    from fpdf import FPDF

    class Doc(FPDF):
        def footer(self):
            self.set_y(-14)
            self.set_font("CJK", "", 8)
            self.set_text_color(*MUTED)
            self.cell(0, 6, f"第 {self.page_no()} 頁 / 共 {{nb}} 頁", align="C")

    pdf = Doc(orientation="P", unit="mm", format="A4")
    pdf.set_auto_page_break(auto=True, margin=18)
    pdf.set_margins(16, 16, 16)
    # 同一個檔案同時當 regular 與 bold:PingFang.ttc 的 bold 面在 fpdf2 取不到,
    # 與其讓它 fallback 成缺字方框,不如用字級與顏色做層次。
    pdf.add_font("CJK", "", font_path)
    pdf.add_font("CJK", "B", font_path)
    pdf.alias_nb_pages()
    pdf.add_page()
    return pdf


def _h1(pdf, text, colour=INK):
    pdf.set_font("CJK", "B", 19)
    pdf.set_text_color(*colour)
    pdf.multi_cell(0, 10, text, align="C")
    pdf.ln(1)
    pdf.set_draw_color(*INK)
    pdf.set_line_width(0.5)
    y = pdf.get_y()
    pdf.line(pdf.l_margin, y, pdf.w - pdf.r_margin, y)
    pdf.ln(4)


def _h2(pdf, text):
    if pdf.get_y() > pdf.h - 45:
        pdf.add_page()
    pdf.ln(3)
    pdf.set_font("CJK", "B", 13)
    pdf.set_text_color(*INK)
    pdf.multi_cell(0, 7, text, align="L")
    pdf.set_draw_color(*RULE)
    pdf.set_line_width(0.2)
    y = pdf.get_y() + 0.5
    pdf.line(pdf.l_margin, y, pdf.w - pdf.r_margin, y)
    pdf.ln(3)


def _h3(pdf, text, colour=INK):
    if pdf.get_y() > pdf.h - 40:
        pdf.add_page()
    pdf.ln(2)
    pdf.set_font("CJK", "B", 11)
    pdf.set_text_color(*colour)
    pdf.multi_cell(0, 6, text, align="L")
    pdf.ln(0.5)


def _para(pdf, text, size=9.5, colour=INK, gap=2.0):
    pdf.set_font("CJK", "", size)
    pdf.set_text_color(*colour)
    # align="L" 是關鍵:fpdf 的預設是左右對齊,中文句子裡只要夾一個 ASCII 詞,
    # 那唯一的空白就會被拉滿整行。
    pdf.multi_cell(0, 5.2, text, align="L")
    pdf.ln(gap)


def _table(pdf, header, rows, widths=None, state_col=None, size=9):
    """畫一個表格。state_col 指定哪一欄要依判定上色。"""
    if not rows:
        return
    avail = pdf.w - pdf.l_margin - pdf.r_margin
    ncols = len(rows[0])
    if widths is None:
        widths = [avail / ncols] * ncols
    else:
        total = sum(widths)
        widths = [w / total * avail for w in widths]

    def cell_lines(text, width):
        pdf.set_font("CJK", "", size)
        text = str(text)
        if pdf.get_string_width(text) <= width - 3:
            return [text]
        # 逐字量測:中文沒有空白可斷,只能一個字一個字放。
        lines, cur = [], ""
        for ch in text:
            if pdf.get_string_width(cur + ch) > width - 3:
                if cur:
                    lines.append(cur)
                cur = ch
            else:
                cur += ch
        if cur:
            lines.append(cur)
        return lines

    line_h = 5.0

    if header:
        pdf.set_font("CJK", "B", size)
        pdf.set_fill_color(*INK)
        pdf.set_text_color(255, 255, 255)
        for w, text in zip(widths, header, strict=False):
            pdf.cell(w, 6.5, f" {text}", fill=True, border=0)
        pdf.ln(6.5)

    for i, row in enumerate(rows):
        wrapped = [cell_lines(c, w) for c, w in zip(row, widths, strict=False)]
        height = max(len(c) for c in wrapped) * line_h + 1.6
        if pdf.get_y() + height > pdf.h - pdf.b_margin:
            pdf.add_page()
            if header:
                pdf.set_font("CJK", "B", size)
                pdf.set_fill_color(*INK)
                pdf.set_text_color(255, 255, 255)
                for w, text in zip(widths, header, strict=False):
                    pdf.cell(w, 6.5, f" {text}", fill=True, border=0)
                pdf.ln(6.5)
        top = pdf.get_y()
        if i % 2 == 0:
            pdf.set_fill_color(*BAND)
            pdf.rect(pdf.l_margin, top, avail, height, style="F")
        x = pdf.l_margin
        for j, (w, lines) in enumerate(zip(widths, wrapped, strict=False)):
            colour = INK
            font = ""
            if state_col is not None and j == state_col:
                colour = STATE_COLOUR.get(
                    next((k for k, v in VERDICT_ZH.items() if v == row[j]), None), INK
                )
                font = "B"
            pdf.set_font("CJK", font, size)
            pdf.set_text_color(*colour)
            for k, line in enumerate(lines):
                pdf.set_xy(x + 1.5, top + 0.8 + k * line_h)
                pdf.cell(w - 2, line_h, line)
            x += w
        pdf.set_y(top + height)
    pdf.set_text_color(*INK)
    pdf.ln(2)


def build_pdf(rep, meta: dict, path: pathlib.Path) -> tuple[pathlib.Path | None, str]:
    """直接從結果資料畫 PDF。永不丟例外 —— 產不出 PDF 只是一則附註。"""
    try:
        from fpdf import FPDF  # noqa: F401
    except ImportError:
        return None, "缺少 fpdf2(pip install -r test/hil/requirements.txt)"
    font = _find_font()
    if not font:
        return None, f"找不到 CJK 字型(找過 {', '.join(CJK_FONTS)})"

    try:
        s = _summary(rep)
        pdf = _new_pdf(font)

        _h1(
            pdf,
            f"SQC485Iv2 QC 驗收報告 — {'通過' if s['passed'] else '未通過'}",
            GREEN if s["passed"] else RED,
        )
        _para(pdf, meta["started_human"], size=9, colour=MUTED, gap=1.0)
        _para(pdf, s["headline"], size=10.5)

        _h2(pdf, "結果")
        _table(
            pdf,
            ["判定", "通過", "失敗", "略過", "未執行", "耗時"],
            [[
                "通過" if s["passed"] else "未通過",
                s["tally"][req.PASS],
                s["tally"][req.FAIL],
                s["tally"][req.SKIP],
                s["gaps"],
                f"{meta['elapsed_s']:.0f} 秒",
            ]],
            widths=[2, 1, 1, 1, 1.2, 1.4],
        )
        _para(pdf, SKIP_NOTE, size=9, colour=MUTED)

        _h2(pdf, "受測裝置")
        _table(pdf, None, _dut_rows(meta), widths=[1, 2.6])

        _h2(pdf, "測試台")
        _table(pdf, ["角色", "埠"], _bench_rows(meta), widths=[1, 2.6])
        _para(pdf, f"執行指令:{meta.get('command', '')}", size=8.5, colour=MUTED)

        _h2(pdf, "受測原始碼")
        _table(pdf, None, _source_rows(meta), widths=[1, 3.4])
        if meta.get("git", {}).get("dirty"):
            _para(
                pdf,
                "這次執行時 working tree 有未提交的修改,所以上面那個 commit "
                "並不足以完整識別被測試的內容。",
                size=9,
                colour=AMBER,
            )

        _h2(pdf, "驗收清單")
        area = None
        for r in req.REQUIREMENTS:
            if r.area != area:
                if area is not None:
                    pdf.ln(1)
                area = r.area
                _h3(pdf, zh.AREAS.get(area, area))
                rows_for_area = [
                    [VERDICT_ZH[rep.status[x.id]], x.id, text_for(x.id)[0]]
                    for x in req.REQUIREMENTS
                    if x.area == area
                ]
                _table(
                    pdf,
                    ["判定", "編號", "需求"],
                    rows_for_area,
                    widths=[0.7, 1.0, 6.0],
                    state_col=0,
                )

        bad = [r for r in req.REQUIREMENTS if rep.status[r.id] == req.FAIL]
        if bad:
            _h2(pdf, "未通過項目")
            _para(pdf, OBSERVED_NOTE, size=9, colour=MUTED)
            for r in bad:
                title, why = text_for(r.id)
                _h3(pdf, f"{r.id} — {title}", RED)
                note = rep.notes.get(r.id)
                if note:
                    _para(pdf, f"實測:{note}", size=9, gap=1.2)
                _para(pdf, f"為什麼重要:{why}。", size=9, colour=MUTED)

        unrun = [r for r in req.REQUIREMENTS if rep.status[r.id] == req.NOT_RUN]
        if unrun:
            _h2(pdf, "未執行項目")
            _para(
                pdf,
                "沒有任何檢查認領這些需求。請補上檢查,或明確記錄一次豁免 —— "
                "不要用 --allow-gaps 把閘門洗成綠燈。",
                size=9,
            )
            _table(
                pdf,
                ["編號", "需求"],
                [[r.id, text_for(r.id)[0]] for r in unrun],
                widths=[1, 6],
            )

        skipped = [r for r in req.REQUIREMENTS if rep.status[r.id] == req.SKIP]
        if skipped:
            _h2(pdf, "略過項目")
            _table(
                pdf,
                ["編號", "需求", "原因"],
                [
                    [r.id, text_for(r.id)[0], rep.notes.get(r.id, "—")]
                    for r in skipped
                ],
                widths=[1, 2.6, 3.4],
            )

        if rep.timings:
            _h2(pdf, "量測時間")
            _table(
                pdf,
                ["量測項目", "數值"],
                [[k, f"{v * 1000:.0f} ms"] for k, v in rep.timings.items()],
                widths=[3, 1],
            )

        pdf.output(str(path))
    except Exception as exc:  # noqa: BLE001 — 排版失敗不該讓 QC run 失敗
        return None, f"PDF 產生失敗:{type(exc).__name__}: {exc}"
    return path, ""


# ── 進入點 ───────────────────────────────────────────────────────────────────


def write(rep, meta: dict, directory: pathlib.Path) -> dict:
    """寫出 <dir>/qc-YYYYMMDD-HHMMSS-<node>.md(+ .pdf),回報實際產出了什麼。"""
    directory.mkdir(parents=True, exist_ok=True)
    node = (meta.get("dut", {}).get("node") or "unknown").lstrip("!")
    stamp = meta["started"].strftime("%Y%m%d-%H%M%S")
    md_path = directory / f"qc-{stamp}-{node}.md"
    md_path.write_text(build_markdown(rep, meta), encoding="utf-8")
    pdf_path, why = build_pdf(rep, meta, md_path.with_suffix(".pdf"))
    return {"markdown": md_path, "pdf": pdf_path, "pdf_error": why}


def now() -> tuple[_dt.datetime, str]:
    started = _dt.datetime.now().astimezone()
    return started, started.strftime("%Y-%m-%d %H:%M:%S %Z")
