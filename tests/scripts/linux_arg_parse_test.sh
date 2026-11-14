#!/bin/sh
# linux_arg_parse_test.sh -- the Linux frontend must REFUSE an argument it cannot
# act on, and name it, instead of running something else and saying nothing.
#
# WHY THIS EXISTS. Two separate ways to lose an argument were live on the shipped
# binary at the start of this batch, and both were measured, not imagined:
#
#   1. An unrecognized argument fell through the whole if/else chain silently and
#      the run continued on defaults. Batch 168 then measured "levels 1-7 at 300
#      frames each" by passing `--level $L` -- a space, where the option wants an
#      equals -- and received level 3 seven times. The draw counts were all
#      identical, which is the tell, and nothing read it. The conclusion happened
#      to survive (levels 3-8 really do all emit zero refusals, re-measured with
#      `--level=`) but the method did not.
#
#   2. An option value that failed to parse became 0, and every reader here takes
#      0 to mean "no limit" or "never". `--frames abc` and `--frames 0` both ran
#      UNBOUNDED -- a harness that asks for a bounded run and then never stops.
#      `--capture=p.bmp --capture-frame=abc` exited 0 having written no file, and
#      `--capture=` did the same: a capture harness reporting success with no
#      picture. `atoi` answers 0 for "abc", "0" and "" alike, which is how three
#      options ended up with three different answers to "is this a number?" while
#      `--size` and `--seed` were validated and these two were not checked at all.
#
#   3. A `--capture` path that PARSED and could not be written. Batch 170 closed the
#      half where the frame the capture was pinned to could not be reached; the half
#      that remained was measured on that batch's own binary: `--capture` into a
#      directory that does not exist, or onto a path that is a directory, printed
#      `[vk] screenshot: could not open ...`, returned false, and `main` returned 0
#      anyway. That is the same lie as (2) -- a harness reporting a frame it did not
#      take -- reached from a perfectly well-formed argument, and it is worse than (2)
#      because the harness upstream then finds the BMP the PREVIOUS run left at that
#      path and commits it as this build's frame. The frontend now asks the renderer
#      how many pictures reached the disk and fails the run when it asked for one and
#      got none (`mdk_render_capture_stats`).
#
#   4. An EXIT CODE DOES NOT UN-WRITE A DESTINATION. (3) makes the run say it took no
#      picture; it does not remove the picture the previous run left at the path, and
#      review-3 measured exactly that: seed a frame at the target, make the write
#      fail, and the run exits 1 with the OLD frame still sitting there at the sha the
#      manifest expects. A harness that keys on the file rather than the exit code --
#      and `expect_capture` below is one -- commits another run's bytes. So the
#      frontend now clears a regular file at the destination when it commits to the
#      request, the same way `tests/frames/capture_oracle.sh` runs `rm -f "$OUT"`
#      before it runs the oracle at all, and the backend publishes a picture by
#      rename so a half-written BMP can never be the thing at the path. The arms for
#      that are below, and they are the only ones here that seed a file first: every
#      other capture arm starts from an empty path and therefore could not see a
#      stale one survive.
#
# A harness that reports having run what it did not run is worse than one that
# fails, because the number looks measured. So every row here is a RUNTIME row:
# "this binary, given that argument, exits non-zero and says why". A shape grep
# cannot establish that, and neither can reading the source.
#
# The class is pinned from BOTH sides. The refuse arms stop the chain from
# silently accepting; the accept arms stop it from being bought by refusing
# everything; the frame-count arm proves a value that parsed is also HONOURED;
# and the capture arm proves a picture actually came out, because "did not
# refuse" and "did the thing asked" are different claims.
#
#   usage: sh tests/scripts/linux_arg_parse_test.sh [repo-root]
#
# Skips (rc 0, with the reason) when the Linux binary is absent, because the gate
# set must stay runnable on a lane that has not built the Linux target -- a skip
# says so and is not a pass. The refuse arms need neither the game data nor a
# display: a refusal happens before anything is initialised. Only the accept arms
# do, and they are reported as skipped rather than quietly dropped.

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP LinuxArgParse (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi

WORK=$(mktemp -d /tmp/mdk-argparse.XXXXXX) || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

export DISPLAY="${DISPLAY:-:99}"
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi
export MDK_DATA_DIR="$DATA"

have_data=0
[ -d "$DATA" ] && have_data=1

pass=0
fail=0
skipped=0
note() {
    if [ "$1" = ok ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL $2"
    fi
}

# Did the refusal name the thing that is wrong? Checked with `case` and not with
# `printf '%s\n' "$out" | grep -q`: a pipe into an early-exit consumer is a
# SIGPIPE race under pipefail, which is a defect this loop has already been
# reviewed for twice in another harness.
names_the_problem() {
    case "$1" in
        *unrecognized*|*"must be"*|*"must name"*|*"needs a value"*|*invalid*) return 0 ;;
        *) return 1 ;;
    esac
}

# expect_reject <label> <args...>: must exit non-zero AND name the offending text.
#
# The watchdog is load-bearing, not hygiene. Two of these arms make the PRE-fix
# binary run unbounded, so without `timeout` the non-vacuity run would hang
# instead of failing, and a hanging test is a worse instrument than a wrong one.
expect_reject() {
    label=$1
    shift
    out=$(timeout 60 "$BIN" "$@" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        note no "[$label] exited 0; it accepted $(printf '%s ' "$@") instead of refusing it"
    elif [ "$rc" -eq 124 ]; then
        note no "[$label] ran until the 60 s watchdog killed it: $(printf '%s ' "$@") was accepted as no limit at all"
    elif ! names_the_problem "$out"; then
        note no "[$label] exited $rc without saying why: $(printf '%s' "$out" | tail -1)"
    else
        note ok
    fi
}

# expect_accept <label> <args...>: a valid invocation must still run, in both of
# the value styles the CLI actually supports.
expect_accept() {
    label=$1
    shift
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    out=$(timeout 120 "$BIN" "$@" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        note no "[$label] a valid invocation now fails (rc $rc): $(printf '%s' "$out" | tail -1)"
    else
        note ok
    fi
}

# expect_frames <label> <n> <args...>: the count must be HONOURED, not merely
# accepted. A parser that stores the value and then ignores it would pass every
# refuse arm and every accept arm above, and the run would still be wrong.
expect_frames() {
    label=$1
    want=$2
    shift 2
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    out=$(timeout 120 "$BIN" "$@" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        note no "[$label] exited $rc: $(printf '%s' "$out" | tail -1)"
    else
        # matched against the whole log with `case`, not against one piped line:
        # the shutdown lines come after the frame line, and a pipe into an
        # early-exit consumer is the race this file already refuses to use.
        case "$out" in
            *"after $want frames"*) note ok ;;
            *) note no "[$label] asked for $want frames and never reported stopping at $want: $(printf '%s' "$out" | tail -1)" ;;
        esac
    fi
}

# expect_capture <label> <out> <args...>: the run must exit 0 AND actually write
# the file it was told to write. This is the arm the refuse arms cannot buy: a
# frontend that refuses everything would pass every row above.
expect_capture() {
    label=$1
    file=$2
    shift 2
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    rm -f "$file"
    out=$(timeout 120 "$BIN" "$@" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        note no "[$label] exited $rc: $(printf '%s' "$out" | tail -1)"
    elif [ ! -s "$file" ]; then
        note no "[$label] exited 0 and wrote no picture -- the capture it was asked for never happened"
    else
        note ok
    fi
}

# expect_capture_refusal <label> <file> <args...>: a capture the run cannot honour
# must fail the run, name THE CAPTURE as the thing that failed, and leave no
# picture behind.
#
# Why this is not `expect_reject`. `expect_reject` asks "non-zero, and did it name a
# problem", and a run that died in renderer creation answers both for entirely the
# wrong reason -- on a lane with no Vulkan every one of the arms below would go green
# while proving nothing. So the refusal has to name the capture, and the file has to
# still be absent afterwards: the promise being pinned is "the file I named is the
# file I wrote", and both halves of that are checked.
#
# The `rm -f` is guarded rather than bare because one arm's "file" is a directory.
expect_capture_refusal() {
    label=$1
    file=$2
    shift 2
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    [ -f "$file" ] && rm -f "$file"
    out=$(timeout 120 "$BIN" "$@" 2>&1)
    rc=$?
    case "$out" in
        *"capture FAILED"*) named=1 ;;
        *) named=0 ;;
    esac
    if [ "$rc" -eq 0 ]; then
        note no "[$label] exited 0 with no picture: the capture failed and the run reported success anyway"
    elif [ "$named" -eq 0 ]; then
        note no "[$label] exited $rc without saying the CAPTURE is what failed: $(printf '%s' "$out" | tail -1)"
    elif [ -f "$file" ] && [ -s "$file" ]; then
        note no "[$label] refused AND wrote a picture -- the two answers disagree"
    else
        note ok
    fi
}

# --- the destination, not only the verdict ---------------------------------------
#
# Everything above asks "did the run refuse, and did it say so". None of it asks
# "what is at the path now", and the path is the only thing an upstream capture
# harness can look at: it cannot read this run's exit code out of a file. So these
# arms SEED a file first -- a stale frame from a previous run -- and then check what
# is there afterwards. Every other capture arm here starts from an empty path and
# therefore could not see a stale one survive; that is the gap review-3 F1 fell
# through, and `expect_capture_refusal`'s own `rm -f` made it worse by wiping the
# evidence before the run even started.
#
# All four are permission-free for the reason the two arms below give: a chmod-based
# repro goes red on a root lane for a reason that has nothing to do with the code.

# expect_stale_cleared <label> <file> <args...>: the run is made unable to produce a
# picture at all -- no usable Vulkan ICD, so the renderer itself cannot be created,
# which is the earliest failure there is and the one no write-side fix can reach --
# and the stale bytes must be GONE afterwards.
#
# The middle test is what stops this arm from going green on a lane with no Vulkan:
# if the run did not fail where the arm made it fail, the arm proved nothing and says
# so rather than passing.
expect_stale_cleared() {
    label=$1
    file=$2
    shift 2
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    printf 'STALE FRAME -- a previous run, not this one\n' > "$file" || {
        note no "[$label] could not seed a stale file at $file"; return 0; }
    out=$(timeout 120 env VK_ICD_FILENAMES=/nonexistent "$BIN" "$@" 2>&1)
    rc=$?
    case "$out" in
        *"renderer creation FAILED"*) broke=1 ;;
        *) broke=0 ;;
    esac
    if [ "$rc" -eq 0 ]; then
        note no "[$label] exited 0 although no Vulkan ICD could be opened"
    elif [ "$broke" -eq 0 ]; then
        note no "[$label] did not fail where it was made to (no renderer-creation failure in the log), so it says nothing about the stale file: $(printf '%s' "$out" | tail -1)"
    elif [ -e "$file" ]; then
        note no "[$label] took no picture and the stale bytes are still at $file -- a harness that keys on the file would commit another run's frame as this build's"
    else
        note ok
    fi
}

# expect_stale_kept <label> <file> <args...>: the mirror image, and the arm that
# keeps the fix from being too eager. An invocation the parser REFUSES never starts,
# so it must not touch the file it was going to write: clearing the destination
# BEFORE validating would delete a file on a typo.
expect_stale_kept() {
    label=$1
    file=$2
    shift 2
    printf 'STALE FRAME -- a previous run, not this one\n' > "$file" || {
        note no "[$label] could not seed a stale file at $file"; return 0; }
    out=$(timeout 60 "$BIN" "$@" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        note no "[$label] exited 0; this arm needs an invocation the parser refuses"
    elif [ ! -s "$file" ]; then
        note no "[$label] refused AND deleted the file it was named with -- a refusal must not touch anyone's bytes"
    else
        note ok
    fi
}

# expect_capture_replaces <label> <file> <args...>: the success path, seeded. The
# clearing must not cost the run its picture: the stale stub has to be replaced by a
# real BMP, and no `.part` sibling may survive the rename that published it.
#
# `head -c 2 "$file"` is not the early-exit race this file refuses elsewhere: the
# bytes come from a file the run already closed, not from a pipe a writer is still
# filling, so there is no writer left to SIGPIPE.
expect_capture_replaces() {
    label=$1
    file=$2
    shift 2
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    printf 'STALE\n' > "$file" || {
        note no "[$label] could not seed a stale file at $file"; return 0; }
    out=$(timeout 120 "$BIN" "$@" 2>&1)
    rc=$?
    magic=$(head -c 2 "$file" 2>/dev/null)
    size=$(wc -c < "$file" 2>/dev/null)
    if [ "$rc" -ne 0 ]; then
        note no "[$label] a valid capture now fails (rc $rc): $(printf '%s' "$out" | tail -1)"
    elif [ ! -s "$file" ]; then
        note no "[$label] exited 0 and left an empty file at $file"
    elif [ "$magic" != "BM" ]; then
        note no "[$label] the file at $file does not start with BM: the stale stub survived, or the picture is not a BMP"
    elif [ "${size:-0}" -lt 1000 ]; then
        note no "[$label] wrote $size bytes at $file, which is not a whole 320x240 frame"
    elif [ -e "$file.part" ]; then
        note no "[$label] left the write's scratch file behind at $file.part"
    else
        note ok
    fi
}

# expect_device_untouched <label> <dev> <args...>: the destination is a CHARACTER
# DEVICE, present here only because /dev/full makes a write fail after a successful
# open. The cleanup must not remove it -- unlinking a device node is not clearing a
# stale picture, it is deleting a system file -- and the write's scratch file must
# not survive beside it.
expect_device_untouched() {
    label=$1
    dev=$2
    shift 2
    if [ "$have_data" -eq 0 ]; then
        skipped=$((skipped + 1))
        return 0
    fi
    out=$(timeout 120 "$BIN" "$@" 2>&1)
    rc=$?
    case "$out" in
        *"capture FAILED"*) named=1 ;;
        *) named=0 ;;
    esac
    if [ "$rc" -eq 0 ]; then
        note no "[$label] exited 0 having written to $dev: nothing can be written to $dev"
    elif [ "$named" -eq 0 ]; then
        note no "[$label] did not name the capture as the thing that failed: $(printf '%s' "$out" | tail -1)"
    elif [ ! -c "$dev" ]; then
        note no "[$label] $dev is GONE: the cleanup unlinked a device node instead of a picture"
    elif [ -e "$dev.part" ]; then
        note no "[$label] left the write's scratch file behind at $dev.part"
    else
        note ok
    fi
}

# --- the argument the chain does not know at all ---------------------------------
# 1. The exact mistake batch 168 made: `--level` with a space instead of an equals.
#    Before the fix this ran the DEFAULT level and exited 0.
expect_reject "space-spelled --level" --headless --fixed-step --level 3 --size=320x240 --frames 1
# 2. An option that does not exist.
expect_reject "unknown option" --headless --fixed-step --turbo --frames 1
# 3. A stray positional, which is what a mistyped flag collapses to.
expect_reject "stray positional" --headless --fixed-step level=3 --frames 1

# --- a value that is not the number the option asked for -------------------------
# 4/5. A count that does not parse, and a count of zero. Both used to mean
#      "unbounded" through atoi, which is the opposite of what was asked.
expect_reject "non-numeric --frames" --headless --fixed-step --frames abc
expect_reject "--frames 0" --headless --fixed-step --frames 0
# 6. A negative count, which atoi also reported as 0-adjacent nonsense.
expect_reject "negative --frames" --headless --fixed-step --frames -5
# 7. --frames is the second option that takes a separate value; with none it must
#    say so rather than call a recognised option unrecognized.
expect_reject "--frames with no value" --headless --fixed-step --seed 1 --frames
# 8/9. The capture frame, same two ways. `--capture-frame=abc` used to exit 0 with
#      no file, which is a capture harness reporting a picture it never took.
expect_reject "non-numeric --capture-frame" --headless --fixed-step --size=320x240 --capture-frame=abc --capture="$WORK/a.bmp" --frames 1
expect_reject "--capture-frame 0" --headless --fixed-step --size=320x240 --capture-frame=0 --capture="$WORK/b.bmp" --frames 1
# 10. An empty capture path: capturePath stays empty, the frame loop's own
#     non-empty test fails, nothing is written, and the run exits 0.
expect_reject "empty --capture path" --headless --fixed-step --size=320x240 --capture= --frames 1
# 11/12. --level already refused a non-number, but by accident (atoi made it 0 and
#        0 is outside 3-8) and with a message about the range. Now it parses first.
expect_reject "non-numeric --size" --headless --fixed-step --size=wide --frames 1
expect_reject "out-of-range --level" --headless --fixed-step --level=99 --size=320x240 --frames 1
expect_reject "non-numeric --level" --headless --fixed-step --level=abc --size=320x240 --frames 1
# 13. --seed is the option that already got this right; kept as a row so a
#     regression in the one good spelling is caught too.
expect_reject "--seed with no value" --headless --fixed-step --seed
expect_reject "non-numeric --seed" --headless --fixed-step --seed one --frames 1

# --- a value that parses, but that THIS run can never act on --------------------
# 14/15/16. A capture frame the run cannot reach. Both halves were measured on the
#      binary before this batch: `--capture=f.bmp --capture-frame=99 --frames 2`
#      exited 0 and wrote no file (the frame bound breaks first), and
#      `--capture-frame=1` with no `--capture` also exited 0 having written no file
#      (the loop breaks on the capture frame whether or not a path exists). A
#      capture harness reporting success with no picture is the exact failure this
#      file exists to refuse, and the value PARSED, so no arm above could see it.
#      Row 16 passes the two options in the other order: the check runs AFTER the
#      whole parse, so order must not matter, and without that row a branch-local
#      check would pass 14 and 15 while missing half the class.
# 17/18. Out of range for `int`. On this x86-64 host `long` is 64 bits, so the old
#      `value > INT_MAX` test already caught these; the ERANGE check is for a
#      32-bit-`long` build, where strtol clamps to LONG_MAX == INT_MAX and the
#      bound test cannot fire. These rows pin the REFUSAL, not the mechanism: the
#      mechanism is not observable on this target and is not claimed to be.
expect_reject "capture frame past the frame bound" --headless --fixed-step --size=320x240 --capture="$WORK/past.bmp" --capture-frame=99 --frames 2
expect_reject "capture frame with no capture path" --headless --fixed-step --size=320x240 --capture-frame=1 --frames 2
expect_reject "capture frame past the bound, reversed order" --headless --fixed-step --size=320x240 --frames 2 --capture="$WORK/past2.bmp" --capture-frame=99
expect_reject "--frames past INT_MAX" --headless --fixed-step --frames 99999999999999999999
expect_reject "--seed past INT_MAX" --headless --fixed-step --seed 99999999999999999999 --frames 1

# --- and the things it must still do --------------------------------------------
# 19/20. Both documented value styles, or the gate is just a refusal machine.
expect_accept "documented run" --headless --fixed-step --level=3 --size=320x240 --seed 1 --frames 2
expect_accept "highest shipped level" --headless --fixed-step --level=8 --size=320x240 --seed 1 --frames 2
# 21. The frame count is honoured, not just parsed.
expect_frames "--frames 3 stops at 3" 3 \
    --headless --fixed-step --level=3 --size=320x240 --seed 1 --frames 3
# 22. The capture really happens -- the pinned frame's own spelling.
expect_capture "capture writes a picture" "$WORK/frame.bmp" \
    --headless --fixed-step --level=3 --size=320x240 --seed 1 --capture-frame=1 --capture="$WORK/frame.bmp" --frames 2
# 23. The boundary of the new bound rule: a capture frame EQUAL to the frame count
#     is reachable, because the loop breaks after the frame and not before it, so
#     the rule is `>` and not `>=`. Without this row the new refusal could be
#     written one value too wide and every reject arm above would still pass.
expect_capture "capture on the last frame the bound allows" "$WORK/edge.bmp" \
    --headless --fixed-step --level=3 --size=320x240 --seed 1 --capture="$WORK/edge.bmp" --capture-frame=2 --frames 2

# 24/25. A capture path that parsed perfectly and cannot be written. Both shapes were
#   measured on batch 170's own binary: rc 0, `[vk] screenshot: could not open ...`,
#   no file -- the frame seam discarded the bool the capture already returned.
#
#   Two shapes and not one, because they fail in different places: a missing parent
#   directory fails the open with ENOENT and a path that IS a directory fails it with
#   EISDIR, and a fix that only checked for one of them (a `stat` of the parent, say)
#   would leave the other live.
#
#   Both are deliberately PERMISSION-FREE. A chmod-555 directory reproduces the same
#   lie on this box, but root writes into a read-only directory without asking, so
#   that arm would go red on a root lane for a reason that has nothing to do with the
#   code -- a test that fails on the identity of whoever runs it is a worse instrument
#   than one that covers one fewer syscall.
expect_capture_refusal "capture into a directory that does not exist" "$WORK/missing/x.bmp" \
    --headless --fixed-step --level=3 --size=320x240 --seed 1 --capture="$WORK/missing/x.bmp" --capture-frame=1 --frames 2
mkdir -p "$WORK/as-a-dir.bmp"
expect_capture_refusal "capture onto a path that is a directory" "$WORK/as-a-dir.bmp" \
    --headless --fixed-step --level=3 --size=320x240 --seed 1 --capture="$WORK/as-a-dir.bmp" --capture-frame=1 --frames 2

# --- and the destination itself ---------------------------------------------------
# 26. Review-3 F1, permission-free. A stale frame sits at the path and the run cannot
#     produce a picture at all; the exit code already said "no picture from this
#     run", and this arm is the half that says the PATH must agree. Measured on the
#     parent binary the stale bytes survive at the sha the manifest expects, which is
#     the whole finding: an exit code does not un-write a destination.
expect_stale_cleared "stale frame cleared when no renderer could be created" \
    "$WORK/stale.bmp" --headless --fixed-step --level=3 --size=320x240 --seed 1 \
    --capture="$WORK/stale.bmp" --capture-frame=1 --frames 2
# 27. The same rule from the parser's side. A request the frontend refuses has not
#     started, so clearing the destination has to come AFTER the refusals: a frontend
#     that cleared first would delete a file on a typo, which is a worse bug wearing
#     this batch's clothes.
expect_stale_kept "a refused invocation leaves the destination alone" \
    "$WORK/keep.bmp" --headless --fixed-step --size=320x240 \
    --capture="$WORK/keep.bmp" --capture-frame=99 --frames 2
# 28/29. The two things the clearing must not cost: the picture itself, and a scratch
#     file beside it. 29 also pins that this is a REGULAR-FILE cleanup -- /dev/full is
#     how a write is made to fail on this host and must not become how a device node
#     gets deleted, which `std::remove` would have done.
expect_capture_replaces "a real capture replaces a stale file at the path" "$WORK/replaced.bmp" \
    --headless --fixed-step --level=3 --size=320x240 --seed 1 \
    --capture="$WORK/replaced.bmp" --capture-frame=1 --frames 2
if [ -c /dev/full ]; then
    expect_device_untouched "a capture onto /dev/full fails and leaves /dev/full alone" \
        /dev/full --headless --fixed-step --level=3 --size=320x240 --seed 1 \
        --capture=/dev/full --capture-frame=1 --frames 2
else
    # Not a pass: the lane has no /dev/full, so the arm did not run.
    skipped=$((skipped + 1))
fi

# The summary must not claim arms it did not run. Four arms need a level, and a
# lane with no game data skips them; printing "a real capture still work" over a
# run in which no capture arm executed is a coverage claim that is simply false.
#
# WHAT THE FIRST HALF DOES NOT COVER, and says so: `--size=320x240junk` and
# `--at=1,2,3junk` are ACCEPTED on purpose (batch 169, "STATED, NOT HIDDEN" --
# `sscanf` reads the leading pair and the trailing junk is ignored), so this file
# does not claim that every value the frontend cannot fully use is refused. It
# claims the narrower thing its arms actually pin: a value whose meaning would be
# silently DISCARDED -- an unparseable count read as "no limit", a capture frame
# the run can never reach, a capture file that cannot be written.
if [ "$fail" -eq 0 ]; then
    if [ "$skipped" -gt 0 ]; then
        echo "PASS LinuxArgParse ($pass arms ok, $skipped SKIPPED for want of game data at $DATA: an option value whose meaning this frontend would silently discard is refused and named -- an unparseable count, a capture frame this run can never reach, and a capture file it cannot write, included; the skipped arms are the ones that need a level, and they did NOT run)"
    else
        echo "PASS LinuxArgParse ($pass arms: an option value whose meaning this frontend would silently discard is refused and named -- an unparseable count, a capture frame the run can never reach, and a capture file that cannot be written, included -- while both documented value spellings, a honoured frame count and two real captures (the capture-frame boundary included) still work; a run that was asked for a picture and could not take one leaves NO picture at the path it named, while a refused invocation, a device node and a scratch file are all left alone; a --size or --at with trailing junk is accepted on purpose and is NOT claimed here)"
    fi
    if [ "$have_data" -eq 0 ]; then
        echo "NOTE LinuxArgParse: $skipped arms that need a level were skipped, not passed (no game data at $DATA); the refuse arms above need neither data nor a display"
    fi
    exit 0
fi
echo "FAIL LinuxArgParse ($pass ok, $fail failing, $skipped skipped)"
exit 1
