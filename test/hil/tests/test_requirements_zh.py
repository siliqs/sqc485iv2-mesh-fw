"""The QC report is read in Traditional Chinese, so the translations have to keep
up with the checklist. `report.py` falls back to the English source text when a
translation is missing — which keeps a run from crashing, and would also let a
new requirement quietly ship an English line into a Chinese report. These tests
are what turns that silent fallback into a visible failure.
"""

import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))

import requirements as req  # noqa: E402
import requirements_zh as zh  # noqa: E402


def test_every_requirement_has_a_translation():
    missing = [r.id for r in req.REQUIREMENTS if r.id not in zh.TEXT]
    assert not missing, (
        f"no Chinese text for {missing} — add it to requirements_zh.py, or the "
        "report prints the English source for those rows"
    )


def test_every_translation_has_both_fields():
    incomplete = [
        rid
        for rid, entry in zh.TEXT.items()
        if not entry.get("title") or not entry.get("why")
    ]
    assert not incomplete, f"missing title or why for {incomplete}"


def test_no_translation_for_a_requirement_that_no_longer_exists():
    """A stale entry means a requirement was renamed and the text was left behind."""
    known = {r.id for r in req.REQUIREMENTS}
    orphans = sorted(set(zh.TEXT) - known)
    assert not orphans, f"requirements_zh.py has text for unknown ids: {orphans}"


def test_every_area_and_needs_value_is_translated():
    areas = {r.area for r in req.REQUIREMENTS}
    assert not areas - set(zh.AREAS), f"untranslated area(s): {areas - set(zh.AREAS)}"
    needs = {r.needs for r in req.REQUIREMENTS}
    assert not needs - set(zh.NEEDS), f"untranslated needs value(s): {needs - set(zh.NEEDS)}"


def test_translations_are_actually_chinese():
    """Guards against an entry left as a copy of the English while being 'filled in'."""
    def has_cjk(text):
        return any("一" <= ch <= "鿿" for ch in text)

    english = [
        rid
        for rid, entry in zh.TEXT.items()
        if not has_cjk(entry["title"]) or not has_cjk(entry["why"])
    ]
    assert not english, f"these entries contain no Chinese at all: {english}"
