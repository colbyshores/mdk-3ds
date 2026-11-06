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
#      and the log line are the instrument; a sink that silently stops retiring
#      has to be visible in the guest log, not inferred from silence. There are
#      TWO retirement sites — reap()'s poll, and audioSink()'s re-check of the
#      slot it picked up between the two — and both are pinned, because losing
#      either one under-reports `done` and the ledger stops being a proof
#      (review F5). What the ledger proves is `submitted - done == inDsp`; the
#      RATE is `submitted` against the SECONDS the same line carries,
#      `submitted / (secs x 50)`, and NOT `done` against `submitted` (review F1)
#      and NOT the frame count in place of the clock, which is the same number
#      only at exactly 60 fps (review-2 F3 of run-20261106-062909, closing the
#      header this file used to carry, review-8 F4).
#
# WHAT THIS DOES NOT COVER, stated plainly: it reads the source, so it pins the
# SHAPE of the calls and not the DSP's behaviour. The unit of nsamples is a
# hardware contract that this file pins by measurement recorded in the progress
# log, not by proof from the firmware; the definitive Old-3DS check stays open
# (AGENTS.md P6/P8 — emulator-green is not hardware-pass).
#
# usage: sh ndsp_sink_test.sh [path-to-audio_3ds.cpp] [path-to-main_3ds.cpp]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$ROOT/mdk_3ds/src/audio/audio_3ds.cpp}
MAIN=${2:-$ROOT/mdk_3ds/src/main_3ds.cpp}
[ -f "$SRC" ] || { printf '  FAIL ndsp_sink: cannot read %s\n' "$SRC"; exit 1; }
[ -f "$MAIN" ] || { printf '  FAIL ndsp_sink: cannot read %s\n' "$MAIN"; exit 1; }

python3 - "$SRC" "$MAIN" <<'PY'
import re
import sys

PATH = sys.argv[1]
MAIN_PATH = sys.argv[2]
SRC = open(PATH, errors="replace").read()
MAIN = open(MAIN_PATH, errors="replace").read()


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


def enclosing_brace(text, pos):
    """The index of the `{` that opens the innermost block containing `pos`, or -1.
    Walks back with a brace balance, so the opener of a sibling block is not
    mistaken for this one's. Returns an INDEX and not the opener's line because an
    Allman brace sits alone and its keyword is one line higher (review-12 R11-1)."""
    depth = 0
    i = pos
    while i >= 0:
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                return i
            depth -= 1
        i -= 1
    return -1


# The loop these three statements live in. Naming it is what keeps the guard test
# from flagging legitimate code: "not inside a block" would reject the frame loop
# itself, and a gate that reds correct code is a gate somebody relaxes.
FRAME_LOOP = r"\bwhile\s*\(\s*aptMainLoop\s*\(\s*\)\s*\)"

# `else if` precedes `else` so the pair reads as one guard rather than as `else`.
GUARD_KEYWORD = re.compile(r"\b(if|else if|else|switch|for|while|do)\b")

# A line that ENDS a construct, so the walk below stops there and a statement after
# an ordinary call, a closed block, or a `case`/`default`/`public:` label is never
# read as somebody's body. Written as the negative of "must end in `{`, `}` or `;`"
# on purpose: the positive form reds correct code after a label, and a gate that
# reds correct code is a gate somebody relaxes.
HEADER_STOP = re.compile(r"[;}:]\s*$")

# A condition can wrap, so the walk is bounded rather than unbounded: six non-blank
# lines covers a multi-line header with room to spare and cannot be talked into
# reading across a statement boundary it did not stop at.
HEADER_WALK = 6


def header_above(text, pos):
    """The control-header line whose body begins at or below the line containing
    `pos`, or "" when nothing above it hands a body to the statement.

    It walks more than one line because a condition can wrap:

        if (frame > 0 &&
            frame < 10u)
            call();

    where the line directly above the call is `frame < 10u)` and names no keyword,
    so a one-line look reads that guard as unconditional — the same mistake
    review-12 R11-1 is about, one style further. Comments and strings are already
    blanked by code(), so neither can supply a keyword or hide a header, and blank
    lines are skipped for the same reason."""
    end = text.rfind("\n", 0, pos)
    steps = 0
    while end >= 0 and steps < HEADER_WALK:
        start = text.rfind("\n", 0, end)
        line = text[start + 1:end]
        end = start
        if not line.strip():
            continue
        steps += 1
        # The stop test goes FIRST: `if (paused) return;` names a keyword and ends a
        # statement, and a line that ends a construct is nobody's header.
        if HEADER_STOP.search(line):
            return ""
        if GUARD_KEYWORD.search(line):
            return line
    return ""


def guard_of(text, match):
    """The keyword that guards `match`, or "" for none.

    THREE forms of guard exist, and a whole-line test sees only the first:

        if (c) call();    // shares its line with its condition — `bare` rejects
        if (c) {          // brace on the condition's line — seen by walking back
            call();       // to the opener of the enclosing block
        }
        if (c)            // braceless, and the same guard in Allman style with the
            call();       // `{` alone above: NEITHER line test sees either

    Review-12 R11-1 reproduced the third form — both its braceless and its Allman
    spelling — as mutants that adjudicated PASS against batch 92, whose fix closed
    only the first two. So the header is read from AROUND the statement rather than
    from its own line: the line above it (braceless), the opener of the enclosing
    block (`if (c) {`), and — when that opener is an Allman `{` sitting alone — the
    header above the brace. Each of those is a walk, not a single line, so a wrapped
    condition does not reopen the hole. The frame loop is whitelisted by name in
    every form, because it is the loop these statements are supposed to be in."""
    line_start = text.rfind("\n", 0, match.start()) + 1
    cands = [text[line_start:match.start()]]            # same-line guard
    cands.append(header_above(text, match.start()))     # braceless guard
    bi = enclosing_brace(text, match.start())
    if bi >= 0:
        opener = text[text.rfind("\n", 0, bi) + 1:bi]
        cands.append(opener)                            # `if (c) {`
        if not opener.strip():                          # Allman: `{` alone
            cands.append(header_above(text, bi))
    for c in cands:
        if re.search(FRAME_LOOP, c):
            return ""
        m = GUARD_KEYWORD.search(c)
        if m:
            return m.group(1)
    return ""


def bare(text, pattern):
    """(group(1), guard) for every match of `pattern` that is the WHOLE of its own
    line, where guard is the keyword guarding it ("" when it is unconditional).

    A call that is the body of an `if`, one arm of a `?:`, or the tail of a `&&`
    chain is still exactly one call site in the file, but it is not handed the
    value on the frames it does not run — so a count of call sites cannot tell an
    unconditional feed from a guarded one. Requiring the statement to own its line
    is the same depth-0 test c3d_frame_test.sh applies to the eye loop
    (`eye-loop-statements-unconditional`, review-3 F2 there, review-11 R10-2
    here); `guard_of` is the half that file does not need, and it is needed here
    because a guard owns its line too — in all three brace styles, not just the
    block style c3d_frame_test.sh has to handle (review-12 R11-1). Comments are
    already blanked by code(), so the only thing
    allowed between the statement's `;` and the end of its line is whitespace."""
    out = []
    for mm in re.finditer(pattern, text):
        a = text.rfind("\n", 0, mm.start()) + 1
        b = text.find("\n", mm.end())
        b = len(text) if b < 0 else b
        if text[a:mm.start()].strip() == "" and text[mm.end():b].strip() == "":
            out.append((mm.group(1), guard_of(text, mm)))
    return out


def fed_ok(feeds):
    """True when `feeds` is exactly one unconditional feed."""
    return len(feeds) == 1 and feeds[0][1] == ""


def fed_text(feeds):
    return ", ".join("%s%s" % (n, "" if not g else " (guarded by `%s`)" % g)
                     for n, g in feeds) or "-"


def adjudicate(src, main, quiet=False):
    c = code(src)
    m = code(main)
    raw = main  # the log FORMAT STRING sits in a string literal, which code()
                # blanks out; matching it has to read the raw file.
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
    check("frontend-logs-stats",
          re.search(r"audioStats\s*\(\s*\)", m) is not None
          and re.search(r"submitted=%[0-9a-z]+", raw) is not None,
          "the frontend must publish the sink's ledger in the guest log")
    # The DENOMINATOR must be on the same line as `submitted`, or the reader has
    # to pair two log lines printed for different reasons (F1). And it must be
    # the CLOCK and not the frame count: `frame x 735/882` is the same number
    # only at exactly 60 fps, so a half-rate sink at 30 fps scores 1.00 against
    # it (review F3 of run-20261106-062909). Both are pinned, and each is read
    # out of the ledger's own format STRING rather than out of the file, so
    # deleting one field cannot satisfy the other check by accident.
    #
    # The VALUE, not just the field (review-8 F2). A check that only looks for
    # `secs=` in the format string passes with ANY variable printed there: the
    # frame's own wall delta (~0.017 s) could stand in for the clock, be wrong by
    # three orders of magnitude, and every arm would still adjudicate red. So
    # three clauses, all read out of the shipped text: the field ORDER on the
    # line (frame, then secs, then submitted, so the rate's two operands are
    # adjacent), the ARGUMENT that fills `secs` (audioClock immediately after the
    # frame cast in the printf's argument list), and the ACCUMULATION that makes
    # audioClock the time the mixer was actually given.
    #
    # And the third clause had the same defect as the second (review-9 F1): it was
    # `audioClock\s*\+=\s*elapsed` as a substring, which `audioClock += elapsed *
    # 0.5f;` satisfies and which a following `audioClock *= 0.999;` never even had
    # to hide. Both mutants printed a ledger rate that was wrong by a factor and
    # both adjudicated PASS, so the accumulation was pinned as a SHAPE and not as
    # a value. It was made structural, and review-10 F4 then showed the structure
    # was still two clauses short — both holes reproduced as mutants that
    # adjudicated PASS:
    #   * the window STOPPED at the printf's argument, so a decay placed AFTER the
    #     ledger printf — still once per frame, still wrong — sat outside the
    #     region and was invisible to it;
    #   * "the same elapsed the device got" was prose and nothing more. Handing
    #     the device `elapsed * 0.5f` while accumulating `elapsed` printed a secs
    #     at twice the audio time and the check could not see it.
    # So: the region is now the REST OF THE FUNCTION (this file closes a function
    # with `}` in column 0), a superset of the frame loop, so a write after the
    # printf is inside it; the count is of WRITES, because the printf's own
    # argument is a read and must not be counted; and the identifier the clock
    # accumulates must be the identifier BOTH update calls are handed.
    #
    # Review-11 found the sixth and seventh holes in the same line, both by
    # counting something that a mutation can leave alone: a WRITE count only sees a
    # write that spells `audioClock` (a reference bound to it decays it behind the
    # check's back, R10-1), and a CALL count only sees that a feed exists, not that
    # it runs (a feed inside `if (frame & 1u)` is one call site that is handed
    # nothing on half the frames, R10-2). Both mutants adjudicated PASS against
    # batch 91, and both are arms now. The fixes are that the clock may not be
    # aliased, and that all three statements must own their line. Review-12 R11-1
    # then found that "owns its line" had three spellings and only two were closed:
    # a braceless `if (c)` with the body below it, and the same guard in Allman
    # style, each adjudicated PASS, so the guard is now read from the lines AROUND
    # the statement rather than from the statement's own line alone.
    lits = [s for s in re.findall(r'"((?:[^"\\]|\\.)*)"', main) if "submitted=" in s]
    ledger = "".join(lits)
    check("frontend-logs-the-frame-with-the-ledger",
          re.search(r"frame=%[0-9a-z.]+", ledger) is not None,
          "the ledger line must carry the frame it was printed at, so it can be "
          "matched against the frame line the frontend prints on the same cadence")
    decl = re.search(r"double\s+audioClock\s*=\s*0\.0\s*;", m)
    fend = re.search(r"\n\}", m[decl.end():]) if decl else None
    region = m[decl.end():decl.end() + fend.start()] if decl and fend else ""
    arg = re.search(r"\(\s*unsigned long\s*\)\s*frame\s*,\s*audioClock\s*,", m)
    writes = re.findall(r"audioClock\s*[-+*/%]?=|\baudioClock\s*(?:\+\+|--)", region)
    # An alias is a write the line above cannot see (review-11 R10-1). Inside this
    # function there are exactly two ways to write `audioClock` without spelling
    # its name: take its address, or bind a reference to it. Both are forbidden
    # below, which is what makes the single-write rule a bound on the VALUE and
    # not merely on one spelling of it.
    aliases = re.findall(r"&\s*audioClock\b|\w+\s*&\s*\w+\s*=\s*audioClock\b", region)
    add = bare(region, r"audioClock\s*\+=\s*([A-Za-z_]\w*)\s*;")
    fed_mixer = bare(m, r"\bmixer\.update\(\s*([A-Za-z_]\w*)\s*\)\s*;")
    fed_dev = bare(m, r"\bmixer\.device\(\)\.update\(\s*([A-Za-z_]\w*)\s*\)\s*;")
    check("frontend-logs-the-clock-with-the-ledger",
          re.search(r"frame=%[0-9a-z.]+\s+secs=%[0-9a-z.]+\s+submitted=", ledger)
          is not None
          and arg is not None
          and len(writes) == 1
          and re.search(r"audioClock\s*\+=\s*elapsed\s*;", region) is not None,
          "the rate test is submitted / (secs x 50): the line must carry frame, "
          "secs and submitted in that order, secs must be FILLED BY the "
          "accumulated audioClock rather than any other expression, and from its "
          "declaration to the end of the function audioClock may be written ONCE "
          "and only as `audioClock += elapsed;`. The window deliberately runs "
          "past the printf: a decay or re-scale placed after it is still once per "
          "frame and still makes the printed secs a different quantity from the "
          "audio time. If a second reader of the clock is genuinely needed, give "
          "it its own accumulator rather than moving this one")
    check("frontend-never-aliases-the-clock", not aliases,
          "audioClock is written once, textually, and never through another name: "
          "%d alias(es) `%s`. Taking the clock's address or binding a reference to "
          "it is how a per-frame decay hides from the single-write rule above — "
          "`double& ac = audioClock; ac *= 0.999;` drops the ledger clock 0.1%% per "
          "frame (0.84%% over a 47 s run) while every textual count stays at one, and "
          "the printed secs stops being the audio's time. If a second reader of the "
          "clock is genuinely needed, give it its own accumulator"
          % (len(aliases), "`, `".join(aliases)))
    check("frontend-feeds-the-clock-and-the-device-the-same-elapsed",
          len(add) == 1 and add[0][1] == "" and fed_ok(fed_mixer) and fed_ok(fed_dev)
          and fed_mixer[0][0] == add[0][0] and fed_dev[0][0] == add[0][0],
          "`secs` is only the audio time if it sums what the mixer and the device "
          "were actually handed: the one `audioClock += X;` and both "
          "`mixer.update(X)` / `mixer.device().update(X)` calls must each be the "
          "whole of its own line — unconditional, not the body of an `if` in any of "
          "its three brace styles (same line, `{` on the condition's line, or "
          "braceless/Allman) nor the arm of a `?:` — and must name the same "
          "identifier. Found %d accumulate(s) "
          "`%s`, %d mixer feed(s) `%s`, %d device feed(s) `%s`. Two distinct errors "
          "live here: a feed handed a scaled or different value than the one "
          "accumulated (review-10 F4), and a feed that runs on SOME frames while the "
          "clock accumulates on all of them (review-11 R10-2 — "
          "`if (frame & 1u) mixer.device().update(elapsed);` is still exactly one "
          "call site and makes the rate test divide by a clock at twice the audio "
          "time). Either way the rate this line exists to publish stops meaning "
          "anything"
          % (len(add), fed_text(add), len(fed_mixer), fed_text(fed_mixer),
             len(fed_dev), fed_text(fed_dev)))

    failed = [n for n, ok, _ in checks if not ok]
    return checks, failed


class Res:
    pass


real_checks, real_failed = adjudicate(SRC, MAIN)
real = Res()
real.checks = len(real_checks)
real.failed = real_failed
real.broke = real_failed[0] if real_failed else ""

ARMS = [
    ("status-pre-set-to-queued",
     lambda s, m: (s.replace("s.wave.status = NDSP_WBUF_FREE;",
                             "s.wave.status = NDSP_WBUF_QUEUED;", 1), m)),
    ("nsamples-back-to-halfwords",
     lambda s, m: (s.replace("s.wave.nsamples = (u32)kBufFrames;",
                             "s.wave.nsamples = (u32)kBufHalfwords;", 1), m)),
    ("flush-shortened-to-frames",
     lambda s, m: (s.replace(
         "DSP_FlushDataCache(s.linear, (u32)kBufHalfwords * sizeof(int16_t));",
         "DSP_FlushDataCache(s.linear, (u32)kBufFrames * sizeof(int16_t));", 1), m)),
    ("dropped-stops-being-counted",
     lambda s, m: (s.replace("g_dropped++;", "", 1), m)),
    ("done-stops-being-counted",
     lambda s, m: (s.replace("g_done++;", "", 1), m)),
    # The second retirement site, targeted by its own text so the arm cannot
    # collide with reap()'s identical statement.
    ("done-not-counted-at-the-sink-recheck",
     lambda s, m: (s.replace(
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_inDsp--;\n            g_done++;",
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_inDsp--;", 1), m)),
    ("inDsp-not-decremented-at-the-sink-recheck",
     lambda s, m: (s.replace(
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_inDsp--;\n            g_done++;",
         "slot.inDsp = false;\n            slot.filled = 0;\n            g_done++;", 1), m)),
    ("requeued-stops-being-counted",
     lambda s, m: (s.replace("g_requeued++;", "", 1), m)),
    ("frontend-stops-logging-the-frame-with-the-ledger",
     lambda s, m: (s, m.replace("frame=%lu ", "", 1))),
    # The `%.3f` is literal HERE on purpose (review-11 open question 1). This arm
    # exists to prove the ledger's `secs` field is load-bearing, and the only
    # honest way to delete the field is to delete the text that prints it. If the
    # precision changes again, the arm mutates nothing and the run FAILS with "the
    # mutation changed nothing" — fail-closed, and the fix is to change the arm in
    # the same commit as the format string, having decided the new precision is
    # right. Editing the arm to match a precision nobody thought about is the
    # failure mode this comment is here to name.
    ("frontend-stops-logging-the-clock-with-the-ledger",
     lambda s, m: (s, m.replace("secs=%.3f ", "", 1))),
    # The substitution case the presence-only check could not see (review-8 F2):
    # the field stays on the line and the value printed is the frame's own wall
    # delta instead of the accumulated clock.
    ("frontend-logs-a-stand-in-for-the-clock",
     lambda s, m: (s, m.replace("(unsigned long)frame, audioClock,",
                                "(unsigned long)frame, (double)delta,", 1))),
    ("frontend-stops-logging-the-ledger",
     lambda s, m: (s, m.replace("submitted=%u", "x=%u", 1))),
    # The accumulation substitutions review-9 F1 produced, both of which the
    # substring clause scored as a PASS: the field stays, the argument stays, and
    # the clock runs at half speed (or decays) behind them.
    ("frontend-scales-the-clock",
     lambda s, m: (s, m.replace("audioClock += elapsed;",
                                "audioClock += elapsed * 0.5f;", 1))),
    ("frontend-decays-the-clock-after-adding",
     lambda s, m: (s, m.replace("audioClock += elapsed;",
                                "audioClock += elapsed; audioClock *= 0.999;", 1))),
    # Review-10 F4: the two holes in the review-9 hardening, each reproduced as a
    # mutant that adjudicated PASS. The first sits AFTER the ledger printf, which
    # is where the old window stopped; the second leaves the clock alone and
    # scales what the device is fed instead.
    ("frontend-decays-the-clock-after-the-ledger",
     lambda s, m: (s, m.replace(
         "(unsigned)st.dspFrames, (unsigned)st.dspDropped);",
         "(unsigned)st.dspFrames, (unsigned)st.dspDropped);\n            "
         "audioClock *= 0.999;", 1))),
    ("frontend-feeds-the-device-a-scaled-elapsed",
     lambda s, m: (s, m.replace("mixer.device().update(elapsed);",
                                "mixer.device().update(elapsed * 0.5f);", 1))),
    ("frontend-feeds-the-mixer-a-different-clock",
     lambda s, m: (s, m.replace("mixer.update(elapsed);", "mixer.update(delta);", 1))),
    # Review-11 R10-1 and R10-2, each reproduced as a mutant that adjudicated PASS
    # against the review-10 hardening. The first writes the clock through a
    # reference, so no textual write count changes; the other three guard one of
    # the three statements, so no call-site count changes and no identifier moves.
    ("frontend-decays-the-clock-through-an-alias",
     lambda s, m: (s, m.replace("audioClock += elapsed;",
                                "audioClock += elapsed; "
                                "{ double& ac = audioClock; ac *= 0.999; }", 1))),
    ("frontend-feeds-the-device-only-some-frames",
     lambda s, m: (s, m.replace("mixer.device().update(elapsed);",
                                "if (frame & 1u) mixer.device().update(elapsed);", 1))),
    ("frontend-feeds-the-mixer-only-some-frames",
     lambda s, m: (s, m.replace("mixer.update(elapsed);",
                                "if (frame & 1u) mixer.update(elapsed);", 1))),
    ("frontend-accumulates-the-clock-only-some-frames",
     lambda s, m: (s, m.replace("audioClock += elapsed;",
                                "if (frame & 1u) audioClock += elapsed;", 1))),
    # The same guard in its block form, which owns its line and so is invisible to
    # a whole-line test on its own. The plausible version of it is "only advance
    # the audio clock while the game is not paused", and that is a guard.
    ("frontend-accumulates-the-clock-in-a-guarded-block",
     lambda s, m: (s, m.replace(
         "audioClock += elapsed;",
         "if (frame != 0u) {\n            audioClock += elapsed;\n        }", 1))),
    ("frontend-feeds-the-device-in-a-guarded-block",
     lambda s, m: (s, m.replace(
         "mixer.device().update(elapsed);",
         "if (frame != 0u) {\n            mixer.device().update(elapsed);\n        }",
         1))),
    # Review-12 R11-1: the same guard in the two styles batch 92 could not see.
    # A braceless body and an Allman brace both own their line, and neither has a
    # `{` on a keyword line for the block walk to land on, so all three of these
    # adjudicated PASS against batch 92 and the first two are the reviewer's own
    # mutants G and H. The plausible code is the same as above — "only feed the
    # device while not paused" — written in the style the author prefers.
    ("frontend-feeds-the-device-in-a-braceless-guard",
     lambda s, m: (s, m.replace(
         "mixer.device().update(elapsed);",
         "if (frame & 1u)\n            mixer.device().update(elapsed);", 1))),
    ("frontend-feeds-the-mixer-in-a-braceless-guard",
     lambda s, m: (s, m.replace(
         "mixer.update(elapsed);",
         "if (frame & 1u)\n            mixer.update(elapsed);", 1))),
    ("frontend-accumulates-the-clock-in-a-braceless-guard",
     lambda s, m: (s, m.replace(
         "audioClock += elapsed;",
         "if (frame & 1u)\n            audioClock += elapsed;", 1))),
    ("frontend-accumulates-the-clock-in-an-allman-guard",
     lambda s, m: (s, m.replace(
         "audioClock += elapsed;",
         "if (frame & 1u)\n        {\n            audioClock += elapsed;\n        }",
         1))),
    ("frontend-feeds-the-device-in-an-allman-guard",
     lambda s, m: (s, m.replace(
         "mixer.device().update(elapsed);",
         "if (frame & 1u)\n        {\n            mixer.device().update(elapsed);\n        }",
         1))),
    # The same guard with a condition that WRAPS, which is why the header is read by
    # a walk and not by one line: the line directly above the statement is then
    # `frame < 999999u)`, which names no keyword at all.
    ("frontend-accumulates-the-clock-in-a-wrapped-guard",
     lambda s, m: (s, m.replace(
         "audioClock += elapsed;",
         "if (frame > 0u &&\n            frame < 999999u)\n            "
         "audioClock += elapsed;", 1))),
]

print("ndsp_sink: %s — %d checks, %d mutation arms" % (PATH, real.checks, len(ARMS)))
for name, ok, detail in real_checks:
    if not ok:
        print("  FAIL ndsp_sink %s: %s" % (name, detail))

arms_red = 0
for label, mutate in ARMS:
    ms, mm = mutate(SRC, MAIN)
    if ms == SRC and mm == MAIN:
        print("  FAIL ndsp_sink arm %s: the mutation changed nothing, so the arm tested the "
              "file it was handed instead of the invariant" % label)
        continue
    _, failed = adjudicate(ms, mm)
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
