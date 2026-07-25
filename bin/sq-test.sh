#!/usr/bin/env bash
#
# sq-test.sh — the Siliqs regression entry point.
#
#   ./bin/sq-test.sh              host unit tests only          (~1 s)
#   ./bin/sq-test.sh --build      + firmware build regression   (minutes)
#   ./bin/sq-test.sh --hil        + hardware in the loop        (needs a board)
#   ./bin/sq-test.sh --all        everything
#
# Anything after `--` is forwarded to the HIL runner, e.g.
#   ./bin/sq-test.sh --hil -- --device /dev/cu.usbmodem844201 --skip-rs485
#
# The default is deliberately the fast one: a check that takes a second gets run
# on every edit, and a check that takes five minutes gets run before a release.

set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

RUN_UNIT=1
RUN_BUILD=0
RUN_HIL=0

HIL_ARGS=()
forwarding=0

for arg in "$@"; do
    if [[ $forwarding -eq 1 ]]; then
        HIL_ARGS+=("$arg")
        continue
    fi
    case "$arg" in
    --) forwarding=1 ;; # everything after this goes to sq_hil.py
    --build) RUN_BUILD=1 ;;
    --hil) RUN_HIL=1 ;;
    --all)
        RUN_BUILD=1
        RUN_HIL=1
        ;;
    -h | --help)
        sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

BOLD=$'\033[1m' OFF=$'\033[0m'
step() { printf '\n%s── %s %s\n' "$BOLD" "$1" "$OFF"; }

if [[ $RUN_UNIT -eq 1 ]]; then
    step "host unit tests (firmware_core)"
    make -C test/sq_core test
fi

if [[ $RUN_BUILD -eq 1 ]]; then
    step "firmware build regression"
    ./bin/sq-build-check.sh
fi

if [[ $RUN_HIL -eq 1 ]]; then
    step "hardware in the loop"

    # The HIL rig needs meshtastic + pyserial, which the rest of the repo does not.
    # Prefer an explicit interpreter, then the rig's own venv, then whatever python3
    # is on PATH — and say how to build the venv rather than failing on an import.
    if [[ -n "${SQ_HIL_PYTHON:-}" ]]; then
        HIL_PY="$SQ_HIL_PYTHON"
    elif [[ -x test/hil/.venv/bin/python ]]; then
        HIL_PY=test/hil/.venv/bin/python
    else
        HIL_PY=python3
    fi

    if ! "$HIL_PY" -c 'import meshtastic, serial' 2>/dev/null; then
        echo "  ${HIL_PY} is missing meshtastic / pyserial. Set one up with:" >&2
        echo "    python3 -m venv test/hil/.venv" >&2
        echo "    test/hil/.venv/bin/pip install -r test/hil/requirements.txt" >&2
        echo "  or point SQ_HIL_PYTHON at an interpreter that already has them." >&2
        exit 1
    fi

    "$HIL_PY" test/hil/sq_hil.py ${HIL_ARGS+"${HIL_ARGS[@]}"}
fi
