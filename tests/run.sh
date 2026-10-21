#!/bin/sh
# Host harness for the c6_bulb firmware: generate harness.c, compile it with gcc -Wall -Wextra, run it.
#   run.sh --old | --old-normal | --new | --board | --all | --compile
# Output (date on line 1, the commands, the output, "exit code: N" last) is the shape of a checks/ file.
# Nothing is written inside the repository: everything goes to $HARNESS_OUT (default /tmp/c6-harness-3.0).
# BULB_C, SETTINGS_C, SETTINGS_H (and STATE_C, EVENTS_C, LIGHT_C, AUTH_C, LED_C, DISC_C, BASE_C, WIFI_C, BUTTON_C, IMPROV_C,
# PORTAL_C, DNS_C, C6_COMP) override the firmware sources; the mutation check instead runs a copy's own tests/run.sh.
# HARNESS_PROFILE=neutral|e11 (default neutral) picks the identity profile the firmware is compiled for; the environment reaches build_harness.py as it is.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
C6=$(dirname "$HERE")
BULB=${BULB_C:-$C6/bulb/main/bulb.c}
SETT=${SETTINGS_C:-$C6/bulb/main/api/v1/settings.c}
SETTH=${SETTINGS_H:-$(dirname "$SETT")/settings.h}
OUT=${HARNESS_OUT:-/tmp/c6-harness-3.0}
VENDOR=$HERE/vendor
export PYTHONDONTWRITEBYTECODE=1
case "$OUT" in "$C6"|"$C6"/*) echo "HARNESS_OUT must be outside $C6" >&2; exit 2;; esac
mode=${1:-}
case "$mode" in --old|--old-normal|--new|--board|--all|--compile) ;; *) echo "usage: run.sh --old | --old-normal | --new | --board | --all | --compile" >&2; exit 2;; esac
for tool in python3 gcc; do command -v $tool >/dev/null 2>&1 || { echo "$tool is missing (apk add python3 gcc musl-dev)"; echo "exit code: 127"; exit 127; }; done
mkdir -p "$OUT"
date -u '+%Y-%m-%d %H:%M UTC'
echo "\$ run.sh $mode   (HARNESS_OUT=$OUT)"
echo "\$ python3 $HERE/build_harness.py $BULB $SETT $SETTH"
python3 "$HERE/build_harness.py" "$BULB" "$SETT" "$SETTH" > "$OUT/extract.txt" 2>&1 || { cat "$OUT/extract.txt"; echo "exit code: 1"; exit 1; }
echo "  ($(grep -c 'extracted from' "$OUT/extract.txt") pieces extracted; list in $OUT/extract.txt)"
echo "\$ gcc -w -c -I $VENDOR -o $OUT/cJSON.o $VENDOR/cJSON.c      (vendored cJSON, not ours: warnings off)"
gcc -w -c -I "$VENDOR" -o "$OUT/cJSON.o" "$VENDOR/cJSON.c" || { echo "exit code: 1"; exit 1; }
echo "\$ gcc -w -c -I $VENDOR -o $OUT/http_parser.o $VENDOR/http_parser.c      (vendored IDF http_parser, not ours: warnings off)"
gcc -w -c -I "$VENDOR" -o "$OUT/http_parser.o" "$VENDOR/http_parser.c" || { echo "exit code: 1"; exit 1; }
echo "\$ gcc -Wall -Wextra -I $VENDOR -o $OUT/harness $OUT/harness.c $OUT/cJSON.o $OUT/http_parser.o -lm"
gcc -Wall -Wextra -I "$VENDOR" -o "$OUT/harness" "$OUT/harness.c" "$OUT/cJSON.o" "$OUT/http_parser.o" -lm > "$OUT/compile.txt" 2>&1
rc=$?
grep -E 'warning|error' "$OUT/compile.txt" | sed "s|$OUT/||" | cut -c1-200
echo "  gcc exit $rc; $(grep -c 'warning' "$OUT/compile.txt") warnings, $(grep -c 'error' "$OUT/compile.txt") errors"
if [ $rc -ne 0 ]; then echo "exit code: $rc"; exit $rc; fi
rc=0
run() {
    echo "\$ $OUT/harness $1"
    "$OUT/harness" "$1"
    r=$?
    [ $r -ne 0 ] && rc=$r
}
case "$mode" in
    --old) run old ;;
    --old-normal) run old-normal ;;
    --new) run new ;;
    --board) run board ;;
    --all) run old; run old-normal; run new; run board ;;
    --compile) ;;
esac
echo "exit code: $rc"
exit $rc
