#!/usr/bin/env bash
# Self-test for hw_cycle.sh's verdict logic, with no console involved.
#
# The console loop's three real failures were all in the harness, and each one
# looked like a build failure:
#   * md5 equality as the stop condition reported a partial log as FRESH;
#   * a first run baselined on a missing local file counted 0 session-end
#     markers and called an OLD marker this run's completion (`cycle 0s`);
#   * a failed pull left the previous pull file in place and the post-loop
#     check trusted it (FRESH after the console's HTTP had gone away).
#
# So the verdict logic is exercised here against a fake console on loopback,
# with a stub `3dslink` on PATH: four cases, each asserting the exit code and
# the sentence that carries the diagnosis. No hardware, no emulator, no display.
#
# Usage: mdk_3ds/tools/hw_cycle_selftest.sh
#        HW_CYCLE_UNDER_TEST=<path to an older hw_cycle.sh> ...selftest.sh
# Exits 0 only when every case matches, so pointing the last form at the
# PARENT harness is the non-vacuity check: the cases that name a defect must
# fail there, or they are not testing anything.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CYCLE=${HW_CYCLE_UNDER_TEST:-"$ROOT/mdk_3ds/tools/hw_cycle.sh"}
DSX="$ROOT/mdk_3ds/mdk.3dsx"
WORK=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -f "$DSX" ]; then
    echo "hw_cycle_selftest: SKIP -- $DSX is not built (make -C mdk_3ds first)."
    exit 0
fi

mkdir -p "$WORK/bin"
printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/3dslink"
chmod +x "$WORK/bin/3dslink"
SIZE=$(stat -c %s "$DSX")

# The fake console serves the guest log and the transfer ledger from files the
# case controls. MODE decides whether a session end ever appears, whether the
# log already carries one before the push, and whether the ledger matches.
cat > "$WORK/fake.py" <<'PY'
import http.server, os, sys, threading, time
MODE = sys.argv[1]
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guest.log")
LED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "transfers.log")
SIZE = sys.argv[2]
with open(LOG, "w") as f:
    f.write("--- mdk 3ds session ---\n")
    if MODE == "stale":
        f.write("--- session end ---\n")
with open(LED, "w") as f:
    out = SIZE if MODE != "ledger" else "12345"
    f.write(f"file=mdk.3dsx status=ok bytes_out={out} bytes_in=1 ms=1\n")
start = time.time()
hits = {"n": 0}
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith("mdk.log"):
            hits["n"] += 1
            if MODE == "wedge" and time.time() - start > 6:
                self.close_connection = True
                try:
                    self.connection.close()
                except OSError:
                    pass
                return
            with open(LOG, "rb") as f:
                body = f.read()
        elif self.path.endswith("transfers.log"):
            with open(LED, "rb") as f:
                body = f.read()
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
def grow():
    time.sleep(4)
    with open(LOG, "a") as f:
        for i in range(3):
            f.write(f"frame {i} elapsed=0.0700 steps=1\n")
            f.flush()
            time.sleep(2)
        if MODE in ("ok", "ledger"):
            f.write("exiting after 3 frames: harness autoquit (MDK_AUTOQUIT)\n")
            f.write("--- session end ---\n")
threading.Thread(target=grow, daemon=True).start()
http.server.HTTPServer(("127.0.0.1", 8080), H).serve_forever()
PY

case_run() {
    name=$1 mode=$2 wait=$3 wantrc=$4 wanttext=$5
    out="$WORK/guest-$mode.log"
    cp "$WORK/fake.py" "$WORK/fake-$mode.py"
    ( cd "$WORK" && exec python3 "$WORK/fake-$mode.py" "$mode" "$SIZE" ) > "$WORK/srv-$mode.out" 2>&1 &
    SRV_PID=$!
    for _ in $(seq 1 20); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 http://127.0.0.1:8080/3ds/mdk/mdk.log)" = 200 ] && break
        sleep 0.5
    done
    PATH="$WORK/bin:$PATH" MDK_HW_WAIT="$wait" "$CYCLE" --once --ip 127.0.0.1 --out "$out" \
        > "$WORK/run-$mode.out" 2>&1
    rc=$?
    kill "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
    # The server must be gone before the next case binds :8080 again.
    for _ in $(seq 1 20); do
        ss -ltn 2>/dev/null | grep -q '127.0.0.1:8080 ' || break
        sleep 0.25
    done
    if [ "$rc" != "$wantrc" ]; then
        echo "FAIL $name -- expected rc $wantrc, got $rc"
        sed 's/^/       /' "$WORK/run-$mode.out"
        FAIL=$((FAIL + 1))
        return
    fi
    if ! grep -q "$wanttext" "$WORK/run-$mode.out"; then
        echo "FAIL $name -- rc $rc but the verdict does not say '$wanttext'"
        sed 's/^/       /' "$WORK/run-$mode.out"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "PASS $name -- rc $rc, '$wanttext'"
    PASS=$((PASS + 1))
}

# 1. A run that exits cleanly is FRESH, and only because a NEW marker appeared.
case_run "clean exit is fresh" ok 60 0 "FRESH"
# 2. A marker left by an earlier session is not this run's completion. This is
#    the `cycle 0s, FRESH` defect: a baseline of 0 against a card that has run
#    the game before.
case_run "an older session end is not this run" stale 12 1 "TIMEOUT waiting for a new session end marker"
# 3. HTTP dying mid-run is the post-exit wedge, said plainly, and the partial
#    pull is kept rather than thrown away.
case_run "a wedge is named as a wedge" wedge 15 1 "post-exit wedge, not the build"
# 4. A push the device ledger cannot confirm refuses to measure at all, because
#    a run of an unknown binary is not evidence.
case_run "an unverified push refuses" ledger 30 1 "push UNVERIFIED"

if [ -s "$WORK/guest-stale.log.partial" ]; then
    echo "PASS a timed-out run keeps its partial pull"
    PASS=$((PASS + 1))
else
    echo "FAIL a timed-out run kept nothing to read afterwards"
    FAIL=$((FAIL + 1))
fi

echo "HW_CYCLE SELFTEST: $PASS PASS / $FAIL FAIL"
[ "$FAIL" -eq 0 ]
