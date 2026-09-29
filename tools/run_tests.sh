#!/usr/bin/env bash
# =============================================================================
#  run_tests.sh — automated hardware-accuracy test suite.
#
#  Runs the emulator headless against Blargg's test ROMs. Those ROMs report
#  their results over the serial port, so --serial --break-on-serial-done lets
#  us read a verdict without ever opening a window.
#
#  Usage:  ./tools/run_tests.sh [-q] [filter]
#            -q       quiet: only print the summary table
#            filter   only run tests whose name matches this substring
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."
BIN=${BIN:-./bin/gameboy}
ROMS=roms/tests
TIMEOUT=${TIMEOUT:-40}
QUIET=0
FILTER=""

while [ $# -gt 0 ]; do
    case "$1" in
        -q|--quiet) QUIET=1 ;;
        *) FILTER="$1" ;;
    esac
    shift
done

if [ ! -x "$BIN" ]; then
    echo "error: $BIN not built. Run 'make' first." >&2
    exit 1
fi

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; DIM=$'\033[2m'; RST=$'\033[0m'
[ -t 1 ] || { RED=""; GRN=""; YEL=""; DIM=""; RST=""; }

pass=0; fail=0; skip=0
declare -a FAILED=()

# run_serial <name> <rom>
#   Runs a Blargg-style ROM that reports over the serial port.
run_serial() {
    local name="$1" rom="$2"
    [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]] && return 0
    if [ ! -f "$rom" ]; then
        printf '  %s%-38s SKIP (missing)%s\n' "$YEL" "$name" "$RST"; skip=$((skip+1)); return 0
    fi

    local out rc
    out=$(timeout "$TIMEOUT" "$BIN" --headless --serial --break-on-serial-done "$rom" 2>&1)
    rc=$?

    if [ $rc -eq 124 ]; then
        printf '  %s%-38s TIMEOUT%s\n' "$RED" "$name" "$RST"
        fail=$((fail+1)); FAILED+=("$name (timeout)")
        [ $QUIET -eq 0 ] && printf '%s%s%s\n' "$DIM" "$(echo "$out" | tail -6 | sed 's/^/      /')" "$RST"
        return 0
    fi

    if echo "$out" | grep -q 'Passed'; then
        printf '  %s%-38s PASS%s\n' "$GRN" "$name" "$RST"; pass=$((pass+1))
    else
        printf '  %s%-38s FAIL%s\n' "$RED" "$name" "$RST"
        fail=$((fail+1)); FAILED+=("$name")
        [ $QUIET -eq 0 ] && printf '%s%s%s\n' "$DIM" "$(echo "$out" | tail -12 | sed 's/^/      /')" "$RST"
    fi
}

# run_hash <name> <rom> <frames> <expected-sha1-or-"?">
#   Renders N frames headless and hashes the framebuffer. Used for the acid2
#   PPU conformance ROMs, which draw a picture instead of printing a verdict.
run_hash() {
    local name="$1" rom="$2" frames="$3" want="$4"
    [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]] && return 0
    if [ ! -f "$rom" ]; then
        printf '  %s%-38s SKIP (missing)%s\n' "$YEL" "$name" "$RST"; skip=$((skip+1)); return 0
    fi
    local got
    got=$(timeout "$TIMEOUT" "$BIN" --headless --frames "$frames" --fbhash "$rom" 2>/dev/null | tail -1)
    if [ -z "$got" ]; then
        printf '  %s%-38s FAIL (no output)%s\n' "$RED" "$name" "$RST"
        fail=$((fail+1)); FAILED+=("$name"); return 0
    fi
    if [ "$want" = "?" ]; then
        printf '  %s%-38s hash=%s%s\n' "$YEL" "$name" "$got" "$RST"; skip=$((skip+1))
    elif [ "$got" = "$want" ]; then
        printf '  %s%-38s PASS%s\n' "$GRN" "$name" "$RST"; pass=$((pass+1))
    else
        printf '  %s%-38s FAIL (got %s)%s\n' "$RED" "$name" "$got" "$RST"
        fail=$((fail+1)); FAILED+=("$name")
    fi
}

echo
echo "=== Blargg cpu_instrs (individual) ==================================="
for f in "$ROMS"/cpu_instrs/individual/*.gb; do
    [ -e "$f" ] || continue
    run_serial "cpu_instrs/$(basename "$f" .gb)" "$f"
done

echo
echo "=== Blargg timing & misc ============================================="
run_serial "instr_timing"      "$ROMS/instr_timing/instr_timing.gb"
run_serial "mem_timing"        "$ROMS/mem_timing/mem_timing.gb"
run_serial "mem_timing-2"      "$ROMS/mem_timing-2/mem_timing.gb"
run_serial "halt_bug"          "$ROMS/halt_bug.gb"
run_serial "interrupt_time"    "$ROMS/interrupt_time/interrupt_time.gb"
run_serial "oam_bug"           "$ROMS/oam_bug/oam_bug.gb"

echo
echo "=== Blargg dmg_sound (individual) ===================================="
for f in "$ROMS"/dmg_sound/rom_singles/*.gb; do
    [ -e "$f" ] || continue
    run_serial "dmg_sound/$(basename "$f" .gb)" "$f"
done

echo
echo "=== PPU conformance (acid2) =========================================="
# Reference hashes are recorded by tools/record_hashes.sh once the renderer is
# verified correct by eye against the published reference images.
ACID_DMG="?"; ACID_CGB="?"
[ -f tools/hashes.txt ] && {
    ACID_DMG=$(grep -m1 '^dmg-acid2 ' tools/hashes.txt | awk '{print $2}')
    ACID_CGB=$(grep -m1 '^cgb-acid2 ' tools/hashes.txt | awk '{print $2}')
    : "${ACID_DMG:=?}"; : "${ACID_CGB:=?}"
}
run_hash "dmg-acid2" "$ROMS/dmg-acid2.gb"  90 "$ACID_DMG"
run_hash "cgb-acid2" "$ROMS/cgb-acid2.gbc" 90 "$ACID_CGB"

echo
echo "======================================================================"
printf ' %sPASS %d%s   %sFAIL %d%s   %sSKIP %d%s\n' \
       "$GRN" "$pass" "$RST" "$RED" "$fail" "$RST" "$YEL" "$skip" "$RST"
if [ ${#FAILED[@]} -gt 0 ]; then
    echo " failing:"
    for t in "${FAILED[@]}"; do echo "   - $t"; done
fi
echo "======================================================================"
echo
[ "$fail" -eq 0 ]
