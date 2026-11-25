#!/bin/sh
# ndsp_sink_test.sh — the ndsp wave-buffer contract, gated.
#
# WHY A SOURCE CHECK EXISTS (same reason as c3d_frame_test.sh). audio_3ds.cpp
# includes <3ds.h> and cannot be compiled on a host, so nothing in
# `make -C tests/scripts` can reach the sink's ndsp calls. The defect this file
# pins is not arithmetic that can be moved out of the backend: it is the shape of
# two struct fields handed to a hardware service, and both fixes are one line
# each, which is exactly what makes them revertible without any other gate
# noticing.
#
# WHAT IT PINS, AND WHY EACH LINE IS LOAD-BEARING.
#
#   1. status MUST BE FREE AT THE CALL. `ndspChnWaveBufAdd` does not queue a
#      buffer it believes is already queued. Disassembly of libctru's
#      ndsp-channel.o (r1 = the wavebuf, offset 17 = status):
#
#          +0x48: ldrb ip, [r1, #17] ; sub ip, ip, #1 ; cmp ip, #1 ; bls +0xb8
#          +0x60: mov r3, #1 ; strb r3, [r1, #17] ; str r3, [r1, #20]  ; QUEUED, next = 0
#          ...append to the channel's wave-buffer list...
#          +0xac: strh r3, [r1, #18]     ; sequence_id — LINKING path only
#          +0xb8: add sp, sp, #20 ; pop {lr} ; b LightLock_Unlock  ; the epilogue
#
#      For status QUEUED(1) or PLAYING(2) the branch at +0x54 goes straight to the
#      epilogue at +0xb8: it links NOTHING and assigns no sequence id, because the
#      strh at +0xac is reachable only by falling through from +0x58. It is libctru
#      that sets QUEUED on the way in. The shipped sink pre-set
#      `status = NDSP_WBUF_QUEUED`, so every submit was a no-op: the DSP was handed
#      nothing, nothing ever came back DONE, the queue filled after four buffers and
#      the sink dropped every mix after the first 80 ms of the run. Measured,
#      before the fix: submitted=4 done=0 dropped=2579 inDsp=4 at frame 2580. After
#      it: submitted=2156 done=2153 dropped=12 requeued=1 inDsp=3 at frame 2580
#      (submitted - done == inDsp, and the ledger line in the guest log carries the
#      frame for exactly that check).
#
#   2. nsamples IS IN FRAMES FOR A STEREO FORMAT. libctru's header comment says
#      "PCM16=halfwords" and one devkitPro example follows it, but four stereo
#      examples and the hardware-proven ../forsaken/sound_3ds.c pass frames, and
#      the emulator settles it: with halfwords the DSP retired 1030 buffers while
#      the mix produced ~2050 — exactly half, i.e. 40 ms spent per 20 ms buffer,
#      which is what reading 1764 as 1764 frames does — and it DMA'd twice the
#      slot, running past the linearAlloc on the last one.
#
#   3. THE SINK STAYS OBSERVABLE. The reason defect 1 survived a whole batch is
#      that the evidence was `sink=1` and `plays=1`, which count ndsp starting
#      and mixer voices launched and say nothing about wave buffers. The counters
#      are the instrument (audioStats). There are TWO retirement sites -- reap()'s
#      poll, and audioSink()'s re-check of the slot it picked up between the two
#      -- and both are pinned, because losing either one under-reports `done` and
#      `submitted - done == inDsp` stops being a proof (review F5). The frontend's
#      periodic `audio: frame=... secs=... submitted=...` ledger line, and the
#      checks on its clock, were a harness-build instrument and left with the
#      harness (branch harness/3ds-profiling).
#
# WHAT THIS DOES NOT COVER, stated plainly: it reads the source, so it pins the
# SHAPE of the calls and not the DSP's behaviour. The unit of nsamples is a
# hardware contract that this file pins by measurement recorded in the progress
# log, not by proof from the firmware; the definitive Old-3DS check stays open
# (AGENTS.md P6/P8 — emulator-green is not hardware-pass).
#
# usage: sh ndsp_sink_test.sh [path-to-audio_3ds.cpp]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$ROOT/mdk_3ds/src/audio/audio_3ds.cpp}
[ -f "$SRC" ] || { printf '  FAIL ndsp_sink: cannot read %s\n' "$SRC"; exit 1; }

python3 - "$SRC" <<'PY'
import re
import sys

PATH = sys.argv[1]
SRC = open(PATH, errors="replace").read()


def code(text):
    """The file with comments and string literals blanked out, so prose that
    documents the rule (this file quotes libctru's disassembly in a comment) is
    not read as code."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body(text, name):
    """The brace-matched body of the function/loop whose header matches `name`."""
    m = re.search(name, text)
    if not m:
        return None
    i = text.find("{", m.end())
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def adjudicate(src, quiet=False):
    c = code(src)
    sub = body(c, r"void\s+submit\s*\(") or ""
    checks = []

    def check(name, ok, detail):
        checks.append((name, ok, detail))

    # 1. status is FREE at the call, and nothing sets QUEUED anywhere.
    check("status-free-before-add",
          re.search(r"wave\.status\s*=\s*NDSP_WBUF_FREE\s*;", sub) is not None
          and sub.find("ndspChnWaveBufAdd") > sub.find("wave.status = NDSP_WBUF_FREE"),
          "submit() must set wave.status = NDSP_WBUF_FREE before ndspChnWaveBufAdd: "
          "libctru refuses to link a buffer whose status is QUEUED or PLAYING")
    check("never-pre-sets-queued",
          re.search(r"wave\.status\s*=\s*NDSP_WBUF_QUEUED", c) is None,
          "ndspChnWaveBufAdd sets QUEUED itself; pre-setting it makes the call a no-op")
    check("status-not-pre-set-to-playing",
          re.search(r"wave\.status\s*=\s*NDSP_WBUF_PLAYING", c) is None,
          "same trap as QUEUED: PLAYING also skips the link")

    # 2. nsamples in frames, and the flush still covers the whole slot in bytes.
    check("nsamples-is-frames",
          re.search(r"wave\.nsamples\s*=\s*\(u32\)kBufFrames\s*;", sub) is not None,
          "nsamples must be the frame count (kBufFrames), not interleaved halfwords")
    check("flush-covers-the-slot",
          re.search(r"DSP_FlushDataCache\(\s*s\.linear\s*,\s*\(u32\)kBufHalfwords\s*\*\s*"
                    r"sizeof\(int16_t\)\s*\)", sub) is not None,
          "the cache flush is in BYTES and must keep covering the whole slot")
    check("slot-sized-in-halfwords",
          re.search(r"kBufHalfwords\s*=\s*kBufFrames\s*\*\s*kChannels", c) is not None,
          "the slot size stays frames x channels; the frames/nsamples distinction "
          "depends on that relation being stated once")

    # 3. the counters, and the log line that publishes them.
    check("counts-submitted", re.search(r"g_submitted\+\+", sub) is not None,
          "a submit that is not counted cannot be compared with what the DSP returns")
    check("counts-done", re.search(r"g_done\+\+", body(c, r"void\s+reap\s*\(") or "") is not None,
          "retired buffers must be counted, or a queue that never drains is invisible")
    # The second retirement site, and the only one that can be lost without any
    # other check noticing (review F5): audioSink re-checks the slot it chose and
    # retires it there when it finished after reap()'s poll. Delete that g_done++
    # and every rate read out of the log is wrong by how often the path runs.
    sink_body = body(c, r"void\s+audioSink\s*\(") or ""
    reap_body = body(c, r"void\s+reap\s*\(") or ""
    check("counts-done-at-the-sink-recheck",
          re.search(r"g_done\+\+", sink_body) is not None,
          "audioSink retires a slot that finished after reap()'s poll; if that site "
          "stops counting, done under-reports and submitted - done != inDsp")
    # The invariant the ledger's whole credibility rests on: every g_done++ is
    # paired with a g_inDsp-- at the same site, and g_inDsp++ is with g_submitted++.
    check("ledger-balances",
          re.search(r"g_inDsp\+\+", sub) is not None
          and len(re.findall(r"g_inDsp--", reap_body)) == 1
          and len(re.findall(r"g_inDsp--", sink_body)) == 1,
          "submitted - done == inDsp only holds while each retirement decrements "
          "inDsp at the same site it counts done")
    check("counts-requeued", re.search(r"g_requeued\+\+", sub) is not None,
          "a submit that had to re-configure a drained channel is the "
          "over-consumption signature; uncounted, a channel that drains every "
          "frame reads as a matched one")
    check("counts-dropped",
          re.search(r"g_dropped\+\+", c) is not None,
          "a dropped fill must be counted, or a saturated queue reads as a working sink")
    check("stats-exposed", re.search(r"AudioStats\s+audioStats\s*\(\s*\)", c) is not None,
          "the counters must be readable from the frontend")
    failed = [n for n, ok, _ in checks if not ok]
    return checks, failed


class Res:
    pass


real_checks, real_failed = adjudicate(SRC)
real = Res()
real.checks = len(real_checks)
real.failed = real_failed
real.broke = real_failed[0] if real_failed else ""

ARMS = [
    ("status-pre-set-to-queued",
     lambda s: s.replace("s.wave.status = NDSP_WBUF_FREE;",
                             "s.wave.status = NDSP_WBUF_QUEUED;", 1)),
    ("nsamples-back-to-halfwords",
     lambda s: s.replace("s.wave.nsamples = (u32)kBufFrames;",
                             "s.wave.nsamples = (u32)kBufHalfwords;", 1)),
    ("flush-shortened-to-frames",
     lambda s: s.replace(
         "DSP_FlushDataCache(s.linear, (u32)kBufHalfwords * sizeof(int16_t));",
         "DSP_FlushDataCache(s.linear, (u32)kBufFrames * sizeof(int16_t));", 1)),
    ("dropped-stops-being-counted",
     lambda s: s.replace("g_dropped++;", "", 1)),
    ("done-stops-being-counted",
     lambda s: s.replace("g_done++;", "", 1)),
    # The second retirement site, targeted by its own text so the arm cannot
    # collide with reap()'s identical statement.
    ("done-not-counted-at-the-sink-recheck",
     lambda s: s.replace(
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_inDsp--;\n            g_done++;",
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_inDsp--;", 1)),
    ("inDsp-not-decremented-at-the-sink-recheck",
     lambda s: s.replace(
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_inDsp--;\n            g_done++;",
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_done++;", 1)),
    ("requeued-stops-being-counted",
     lambda s: s.replace("g_requeued++;", "", 1)),
]

print("ndsp_sink: %s — %d checks, %d mutation arms" % (PATH, real.checks, len(ARMS)))
for name, ok, detail in real_checks:
    if not ok:
        print("  FAIL ndsp_sink %s: %s" % (name, detail))

arms_red = 0
for label, mutate in ARMS:
    ms = mutate(SRC)
    if ms == SRC:
        print("  FAIL ndsp_sink arm %s: the mutation changed nothing, so the arm tested the "
              "file it was handed instead of the invariant" % label)
        continue
    _, failed = adjudicate(ms)
    if failed:
        arms_red += 1
        print("  ok   arm %s: turned `%s` red as required" % (label, failed[0]))
    else:
        print("  FAIL ndsp_sink arm %s: the mutation was accepted, so the check does not pin "
              "what it claims to pin" % label)

if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL ndsp_sink: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
print("PASS NdspSink (%d checks, %d arms adjudicated red)" % (real.checks, arms_red))
PY
exit $?
