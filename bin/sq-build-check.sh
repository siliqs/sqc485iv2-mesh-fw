#!/usr/bin/env bash
#
# sq-build-check.sh — firmware build regression for the Siliqs SQC485Iv2 envs.
#
# Three things are checked for every product environment:
#
#   1. it still builds                  — the v231 and usbtunnel SKUs are easy to
#                                         break and easy to forget, since only the
#                                         main env is exercised by day-to-day work
#   2. flash / RAM stayed within budget — growth is fine, silent growth is not
#   3. the certified radio defaults are actually in the image
#
# (3) exists because it has failed before: the Taiwan DTS profile (922.5 MHz /
# BW500 / SF9) was reverted to LongFast by initDefaultLoraConfig() and shipped
# that way. Reading the constant back out of the binary is the only check that
# does not depend on remembering to look.
#
#   ./bin/sq-build-check.sh                      build + check everything
#   ./bin/sq-build-check.sh --no-build           check the existing artifacts
#   ./bin/sq-build-check.sh --update-baseline    accept the current sizes
#   SQ_SIZE_TOLERANCE=5 ./bin/sq-build-check.sh  allow 5% growth (default 2%)

set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ENVS=(
    sqc485iv2-esp32c3-sx1262
    sqc485iv2-v231-esp32c3-sx1262
    sqc485iv2-usbtunnel-esp32c3-sx1262
)

BASELINE="test/baseline/firmware-size.json"
TOLERANCE="${SQ_SIZE_TOLERANCE:-2}"

DO_BUILD=1
UPDATE_BASELINE=0
for arg in "$@"; do
    case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --update-baseline) UPDATE_BASELINE=1 ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

RED=$'\033[31m' GREEN=$'\033[32m' YELLOW=$'\033[33m' DIM=$'\033[2m' OFF=$'\033[0m'
failures=0

pass() { printf '  %s✓%s %s\n' "$GREEN" "$OFF" "$1"; }
fail() {
    printf '  %s✗%s %s\n' "$RED" "$OFF" "$1"
    failures=$((failures + 1))
}
note() { printf '    %s%s%s\n' "$DIM" "$1" "$OFF"; }

# ── the values the product is certified for ──────────────────────────────────
# 922.5f as a little-endian IEEE-754 single: 0x4466A000 -> bytes 00 A0 66 44.
# This is the override_frequency written by both NodeDB::installDefaultConfig()
# and Channels::initDefaultLoraConfig().
DTS_FREQ_BYTES="00a06644"
EXPECT_PRODUCT_ID="SQC485Iv2"

FW_VERSION="$(sed -n 's/^#define SQ_FW_VERSION *"\(.*\)".*/\1/p' src/siliqs/firmware_core/include/config.h)"
if [[ -z "$FW_VERSION" ]]; then
    echo "cannot read SQ_FW_VERSION from config.h" >&2
    exit 1
fi

echo
echo "SQC485Iv2 build regression   ${DIM}fw ${FW_VERSION}, tolerance ${TOLERANCE}%${OFF}"

# ── 1. build ─────────────────────────────────────────────────────────────────
# Sizes are collected into a temp file rather than an associative array: macOS
# still ships bash 3.2, which has none.
SIZES="$(mktemp)"
trap 'rm -f "$SIZES"' EXIT

for env in "${ENVS[@]}"; do
    log=".pio/build/${env}/sq-build.log"
    mkdir -p ".pio/build/${env}"

    if [[ $DO_BUILD -eq 1 ]]; then
        echo
        echo "building ${env}"
        if ! pio run -e "$env" >"$log" 2>&1; then
            fail "${env}: build failed"
            tail -30 "$log" | sed 's/^/    /'
            continue
        fi
        pass "${env}: builds"
    fi

    # PlatformIO prints e.g. "Flash: [====  ] 40.2% (used 843210 bytes from ...)"
    if [[ -f "$log" ]]; then
        flash="$(sed -n 's/^Flash:.*used \([0-9]*\) bytes.*/\1/p' "$log" | tail -1)"
        ram="$(sed -n 's/^RAM:.*used \([0-9]*\) bytes.*/\1/p' "$log" | tail -1)"
        [[ -n "$flash" ]] && echo "${env} ${flash} ${ram:-0}" >>"$SIZES"
    fi
done

# ── 2. certified defaults are present in the image ───────────────────────────
echo
echo "certified radio defaults"

# PlatformIO names artifacts firmware-<env>-<version>.<githash>.bin, so several
# builds accumulate side by side; take the newest and never the factory image
# (which is the app padded with bootloader + partitions).
newest_app_image() {
    ls -t ".pio/build/${1}/firmware-${1}-"*.bin 2>/dev/null | grep -v '\.factory\.bin$' | head -1
}

for env in "${ENVS[@]}"; do
    bin="$(newest_app_image "$env")"
    if [[ -z "$bin" || ! -f "$bin" ]]; then
        fail "${env}: no firmware image found (build first)"
        continue
    fi
    note "$(basename "$bin")"

    # grep -c rather than grep -q: with `set -o pipefail`, an early-exiting grep -q
    # SIGPIPEs the producer and the whole pipeline reports failure even on a match.
    if [[ "$(xxd -p "$bin" | tr -d '\n' | grep -c "$DTS_FREQ_BYTES" || true)" -gt 0 ]]; then
        pass "${env}: 922.5 MHz DTS default present"
    else
        fail "${env}: 922.5 MHz override_frequency NOT in the image"
        note "the node would ship on the LongFast preset — outside the tested envelope"
    fi

    if [[ "$(strings -a "$bin" | grep -cxF "$EXPECT_PRODUCT_ID" || true)" -gt 0 ]]; then
        pass "${env}: product id ${EXPECT_PRODUCT_ID} present"
    else
        fail "${env}: product id string missing — the configurator cannot label this node"
    fi

    if [[ "$(strings -a "$bin" | grep -cxF "$FW_VERSION" || true)" -gt 0 ]]; then
        pass "${env}: reports fw ${FW_VERSION}"
    else
        fail "${env}: SQ_FW_VERSION ${FW_VERSION} not found in the image"
    fi
done

# ── 3. size budget ───────────────────────────────────────────────────────────
echo
echo "size budget"

size_json="$(python3 -c '
import json, sys
out = {}
for line in open(sys.argv[1]):
    env, flash, ram = line.split()
    out[env] = {"flash": int(flash), "ram": int(ram)}
print(json.dumps(out))
' "$SIZES")"

if [[ "$size_json" == "{}" ]]; then
    printf '  %s!%s no size data — rerun without --no-build\n' "$YELLOW" "$OFF"
fi

if [[ $UPDATE_BASELINE -eq 1 ]]; then
    mkdir -p "$(dirname "$BASELINE")"
    python3 - "$BASELINE" "$size_json" "$FW_VERSION" <<'PY'
import json, sys
path, sizes, version = sys.argv[1], json.loads(sys.argv[2]), sys.argv[3]
json.dump({
    "_comment": "Firmware size baseline. Regenerate with ./bin/sq-build-check.sh --update-baseline "
                "when growth is intentional, and say why in the commit message.",
    "recorded_for_version": version,
    "envs": sizes,
}, open(path, "w"), indent=2, sort_keys=True)
open(path, "a").write("\n")
print(f"  baseline written: {path}")
PY
    exit 0
fi

if [[ ! -f "$BASELINE" ]]; then
    printf '  %s!%s no baseline yet — run: ./bin/sq-build-check.sh --update-baseline\n' "$YELLOW" "$OFF"
else
    if ! python3 - "$BASELINE" "$size_json" "$TOLERANCE" <<'PY'; then
import json, sys
base = json.load(open(sys.argv[1]))["envs"]
now = json.loads(sys.argv[2])
tol = float(sys.argv[3]) / 100.0
GREEN, RED, DIM, OFF = "\033[32m", "\033[31m", "\033[2m", "\033[0m"

bad = False
for env, cur in sorted(now.items()):
    ref = base.get(env)
    if not ref:
        print(f"  {DIM}? {env}: not in baseline{OFF}")
        continue
    for kind in ("flash", "ram"):
        was, is_ = ref.get(kind, 0), cur.get(kind, 0)
        if not was:
            continue
        delta = is_ - was
        pct = delta / was * 100.0
        label = f"{env} {kind}: {is_:,} bytes ({pct:+.2f}%, {delta:+,})"
        if delta > was * tol:
            print(f"  {RED}✗{OFF} {label} — over budget")
            bad = True
        else:
            print(f"  {GREEN}✓{OFF} {label}")
sys.exit(1 if bad else 0)
PY
        failures=$((failures + 1))
    fi
fi

# ── 4. release tag consistency ───────────────────────────────────────────────
# The release workflow triggers on sqc485iv2-v* tags, but nothing forces the tag
# and SQ_FW_VERSION to agree — and they have already drifted once.
tag="$(git describe --exact-match --tags --match 'sqc485iv2-v*' 2>/dev/null || true)"
if [[ -n "$tag" ]]; then
    echo
    echo "release tag"
    if [[ "${tag#sqc485iv2-v}" == "$FW_VERSION" ]]; then
        pass "tag ${tag} matches SQ_FW_VERSION ${FW_VERSION}"
    else
        fail "tag ${tag} does not match SQ_FW_VERSION ${FW_VERSION}"
        note "the configurator would report a version this release is not"
    fi
fi

echo
if [[ $failures -gt 0 ]]; then
    printf '%sFAILED%s  %d check(s)\n\n' "$RED" "$OFF" "$failures"
    exit 1
fi
printf '%sPASSED%s  build regression clean\n\n' "$GREEN" "$OFF"
