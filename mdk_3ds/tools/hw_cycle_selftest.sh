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

mkdir -p "$WORK/bin"
# The default payload is a stand-in carrying the harness chainload literal, so
# the verdict cases do not depend on which variant the tree last built (a plain
# `make -C mdk_3ds` would otherwise turn every case into the refusal case).
printf 'stand-in harness payload\0/3ds/netload_stub.3dsx\0' > "$WORK/harness.3dsx"
DSX="$WORK/harness.3dsx"
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
    if MODE == "capold":
        # An EARLIER session wrote a capture; this run will not.
        f.write("screenshot: wrote sdmc:/3ds/mdk/hwcap.bmp.log (240x400)\n--- session end ---\n--- mdk 3ds session ---\n")
    if MODE in ("rotate", "rotbad"):
        # An old card: three finished sessions before this push.
        for i in range(3):
            f.write("frame 1 elapsed=0.0700 steps=1\n--- session end ---\n--- mdk 3ds session ---\n")
with open(LED, "w") as f:
    out = SIZE if MODE != "ledger" else "12345"
    f.write(f"file=mdk.3dsx status=ok bytes_out={out} bytes_in=1 ms=1\n")
PREV_SIZE = os.path.getsize(LOG)
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
        elif self.path.endswith("hwcap.bmp.log"):
            import struct
            px = bytes(4 * 4 * 3)
            body = b"BM" + struct.pack("<IHHI", 54 + len(px), 0, 0, 54) + \
                struct.pack("<IiiHHIIiiII", 40, 4, 4, 1, 24, 0, len(px), 0, 0, 0, 0) + px
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
    if MODE == "rotbad":
        # NOT a rotation: the log is replaced by a body that does not begin
        # with the snapshot, carries old markers, and states no rotation.
        with open(LOG, "w") as f:
            f.write("--- mdk 3ds session ---\n--- session end ---\n--- session end ---\n")
        return
    if MODE == "rotate":
        # The game rotated the oversized log at boot: a fresh file holding
        # ONLY this session, so its marker count (1) is below the baseline (3).
        with open(LOG, "w") as f:
            f.write("--- mdk 3ds session ---\n")
            f.write(f"log: rotated the previous {PREV_SIZE}-byte log to mdk.prev.log\n")
            f.write("exiting after 3 frames: harness autoquit (MDK_AUTOQUIT)\n")
            f.write("--- session end ---\n")
        return
    with open(LOG, "a") as f:
        for i in range(3):
            f.write(f"frame {i} elapsed=0.0700 steps=1\n")
            f.flush()
            time.sleep(2)
        if MODE == "capture":
            f.write("screenshot: wrote sdmc:/3ds/mdk/hwcap.bmp.log (4x4)\n")
        if MODE in ("profold", "profnew"):
            fam = "old3ds" if MODE == "profold" else "new3ds"
            f.write("profile: frames=3 mean=33.300ms p50<=33.4ms p95<=33.4ms p99<=33.4ms max=33.400ms over16.7ms=3 over33.3ms=0\n")
            f.write(f"profile: console model=0 family={fam} clock=268MHz l2=none stereo=off\n")
        if MODE in ("ok", "ledger", "capture", "capold", "profold", "profnew"):
            f.write("exiting after 3 frames: harness autoquit (MDK_AUTOQUIT)\n")
            f.write("--- session end ---\n")
threading.Thread(target=grow, daemon=True).start()
http.server.HTTPServer(("127.0.0.1", 8080), H).serve_forever()
PY

DSX_OVERRIDE=""

case_run() {
    name=$1 mode=$2 wait=$3 wantrc=$4 wanttext=$5
    out="$WORK/guest-$mode.log"
    cp "$WORK/fake.py" "$WORK/fake-$mode.py"
    ( cd "$WORK" && exec python3 "$WORK/fake-$mode.py" "$mode" "$(stat -c %s "${DSX_OVERRIDE:-$DSX}")" ) > "$WORK/srv-$mode.out" 2>&1 &
    SRV_PID=$!
    for _ in $(seq 1 20); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 http://127.0.0.1:8080/3ds/mdk/mdk.log)" = 200 ] && break
        sleep 0.5
    done
    PATH="$WORK/bin:$PATH" MDK_HW_WAKE=0 MDK_HW_WAIT="$wait" MDK_HW_DSX="${DSX_OVERRIDE:-$DSX}" "$CYCLE" "${CYCLE_MODE:---once}" --ip 127.0.0.1 --out "$out" \
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

# 5. The game rotates an oversized log at boot (log_3ds.cpp), so the fresh log
#    can hold FEWER markers than the pre-push baseline. A count-only verdict
#    times out on a run that finished; the harness must see that the log no
#    longer begins with the snapshot and baseline the new file at 0.
case_run "a rotated log is this run" rotate 30 0 "log rotated by the game"
# 5b. A log that merely differs from the snapshot, without the game's rotation
#     line naming the snapshot's size, is not a rotation: its old markers must
#     not read as this run's (review finding H1).
case_run "a log that only differs is not a rotation" rotbad 12 1 "TIMEOUT waiting for a new session end marker"

if [ -s "$WORK/guest-stale.log.partial" ]; then
    echo "PASS a timed-out run keeps its partial pull"
    PASS=$((PASS + 1))
else
    echo "FAIL a timed-out run kept nothing to read afterwards"
    FAIL=$((FAIL + 1))
fi

# 6. A payload that cannot end a run is refused before the push. Without
#    -DMDK_AUTOQUIT the harness links a constant-false deadline, so the only
#    reachable verdict is a timeout on a console still playing the game. This
#    is the binary that WAS pushed on 2026-11-17 (it carries main's
#    `harness autoquit` literal but not the chainload path), so the check reads
#    the chainload path and not the phrase.
: > "$WORK/no-harness.3dsx"
printf 'harness autoquit (MDK_AUTOQUIT)\0' >> "$WORK/no-harness.3dsx"
DSX_OVERRIDE="$WORK/no-harness.3dsx"
case_run "a payload without the harness is refused" ok 30 1 "WITHOUT -DMDK_AUTOQUIT"
DSX_OVERRIDE=""

# 7-9. --capture (hw.pixels-rgba8). A capture payload carries both harness
#    literals; the picture counts only when THIS session logged its write.
printf '/3ds/netload_stub.3dsx\0sdmc:/3ds/mdk/hwcap.bmp.log\0' > "$WORK/capture.3dsx"
DSX_OVERRIDE="$WORK/capture.3dsx"
CYCLE_MODE=--capture
case_run "a capture run pulls its picture" capture 60 0 "CAPTURE 4x4 102 bytes"
case_run "an earlier session's capture is not this run's" capold 60 1 "CAPTURE MISSING"
# A harness payload with neither the capture nor the profile, independent of
# what the tree's own mdk.3dsx happens to be built with.
printf '/3ds/netload_stub.3dsx\0' > "$WORK/plain.3dsx"
DSX_OVERRIDE="$WORK/plain.3dsx"
case_run "a payload without the capture is refused" ok 30 1 "WITHOUT CAPTURE"
CYCLE_MODE=

# 10-12. --profile (hw.profile-og): an Old-3DS summary is the OG result, a New
#    3DS one is refused by name, and a payload without the profile never pushes.
printf '/3ds/netload_stub.3dsx\0profile: console model=%%u\0' > "$WORK/profile.3dsx"
DSX_OVERRIDE="$WORK/profile.3dsx"
CYCLE_MODE=--profile
case_run "an Old-3DS profile is an OG result" profold 60 0 "PROFILE OG"
case_run "a New-3DS profile is not an OG result" profnew 60 1 "NOT AN OG RESULT"
DSX_OVERRIDE="$WORK/plain.3dsx"
case_run "a payload without the profile is refused" ok 30 1 "WITHOUT PROFILE=1"
DSX_OVERRIDE=""
CYCLE_MODE=

echo "HW_CYCLE SELFTEST: $PASS PASS / $FAIL FAIL"
[ "$FAIL" -eq 0 ]
