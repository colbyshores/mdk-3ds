#!/bin/sh
# linux_refusal_report_test.sh — a refused resource on the Linux target must be
# PRINTED, and the number it prints must be the request that was refused.
#
# WHY THIS EXISTS. `object_draw.h` has carried the same sentence since review-11
# F4: "mdk_linux/src/main.cpp calls `draw` without consulting the flag and the
# Vulkan backend's mesh refusal prints nothing either, so a refused mesh on Linux
# is still a silent missing picture. That half is recorded and NOT fixed". A
# refused resource is the worst failure the renderer can have, because it is
# invisible: the frame still renders, the geometry is simply not in it, the
# screenshot looks plausible, and every gate stays green. The Citro3D backend has
# always printed its refusals (`renderer_c3d.cpp`'s `linearAlloc(N) FAILED for a
# dynamic mesh of %d corners`); the Vulkan backend answered the same failures
# with a bare `return -1`, and the Linux frontend never read the flag the shared
# pass records.
#
# WHY A SOURCE CHECK AND NOT A GOLDEN. Neither half is reachable from a harness.
# A refusal needs the device to say no, and lavapipe on this host does not: a
# 1200-frame run of level 3 and of level 7 prints zero `REFUSED` lines, so no
# executed arm can produce one without faking the driver. The frontend's copy of
# the guard sits inside `if (arena)` -> LevelData::load -> spawnPlayer(), behind
# the licensed tree and a renderer — the same reachability wall
# `sprites_refusal_test.sh` records for the Linux asset gate, and the reason
# `main_3ds_flow_test.sh` and `frontend_solid_pin_test.sh` exist for the 3DS side.
# What is load-bearing here is WHERE a call sits and WHAT number it reads, which
# is a shape, not arithmetic. The shape is pinned here; the refusal flag itself
# (`ObjectDraw::allocationFailed`, its clearing, and `attemptedCapacity`) is
# executed by the host suite.
#
# WHAT IS PINNED.
#
#   frontend (mdk_linux/src/main.cpp)
#     F1 the object pass consults `objectDraw.allocationFailed()` at all, and
#        prints inside that test — not beside it, so a refusal cannot be reported
#        on a frame that did not refuse;
#     F2 the line prints the capacity that was REFUSED (`attemptedCapacity()`),
#        not `capacity()`, which `ensure` has already zeroed, and not the frame's
#        corner count, which is up to 8x smaller than the rounded-up request
#        (review-11 F1, the same defect the 3DS frontend's line was corrected for);
#     F3 the line is edge-triggered AND re-armed: the latch is set inside the
#        branch before the print, and cleared in the `else` sibling of the
#        `allocationFailed()` test, so it runs on every frame that did not refuse
#        (review-11 F7: a one-shot latch reports the first, smallest refusal and
#        goes silent about the bigger one that followed; review-12 R1: a check on
#        the assignment line pins a line, not the guard).
#
#   backend (mdk_linux/src/render/renderer_vk.cpp)
#     B1 every refusal site prints: inside each named function, every block that
#        tests a failed create/ensure/over-capacity and returns carries a
#        `refused(` call before that return;
#     B2 the helper actually reaches the log — it formats the caller's varargs
#        and flushes, because a refusal is exactly the moment the process may die
#        with a stdio buffer full of the line that explains it;
#     B3 the byte figure a refusal prints IS the size expression the create call
#        was given, optionally times the frames-in-flight count, and in BOTH
#        directions: a figure that mentions LESS than the size (a bare
#        `kFramesInFlight`, or `cap / 8`) is as red as one that mentions something
#        else. EVERY size site contributes, so one readable site cannot vouch for
#        the others (review-5 F4, review-6 F1);
#     B4 DISCOVERY, not a list: every `return VK_NULL_HANDLE` in the file has a
#        `refused(` attributed to THAT return. B1 can only check the sites it was
#        told about, and review-5 F1 was a site it had not been told about
#        (`getDescriptorSet`, whose callers then drew with the PREVIOUS draw's
#        descriptor set bound — a wrong picture, not a missing one). Attribution is
#        per return and not per function, because an unbraced `if (x) return
#        VK_NULL_HANDLE;` in a function that already refuses once is otherwise
#        vouched for by a refusal that has nothing to do with it (review-6 F3);
#     B5 every `vkAllocate*` call has its VkResult read, because an unchecked
#        `vkAllocateCommandBuffers` hands back an undefined handle that the next
#        call happily dereferences (review-5 F1);
#     B6 `createHostImage` does not report success for an image it never filled
#        or transitioned (review-5 F1);
#     B7 a refusal prints ONCE PER DISTINCT REQUEST. The Citro3D backend latches
#        with a bool and it must, because that line printed 1710 times in one run
#        (review-10 F3); a bool is too coarse here, where a descriptor set is
#        allocated per DRAW. So the helper keeps the SET of requests each site has
#        named and is silent only for one of them — and it has to be a set: a latch
#        that remembers only the LAST request reprints a rotating set of them on
#        every frame, which measured 22500 lines for 48 distinct requests
#        (review-5 F2).
#     B8 the one-shot command buffer and the pool it came from are handed over
#        together, and every failure exit after the pool exists destroys the pool it
#        made. They used to be paired by POSITION in a file-scope vector, and one
#        stranded entry made every later `endOneShot` free a foreign command buffer
#        into the wrong pool (review-6 F2);
#     B9 in a teardown block the memory is released LAST, after the buffer or image
#        bound to it is destroyed. `vkFreeMemory` on memory still bound to a live
#        object is invalid use, and this file had two of them (review-6 F6).
#
# Every check is paired with a mutation arm, and every arm names the one check it
# must turn red (review-7 F2's rule: an arm satisfied by a different check leaves
# its own check with no mutation that isolates it). An arm whose mutation changes
# nothing is a FAIL, not a skip: that is the pass-generator this file exists to
# avoid.
#
# usage: sh linux_refusal_report_test.sh [root-dir]
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}

python3 - "$ROOT" <<'PY'
import re
import sys

ROOT = sys.argv[1]
FRONTEND = "mdk_linux/src/main.cpp"
BACKEND = "mdk_linux/src/render/renderer_vk.cpp"

# The backend functions that can refuse, and the pattern that marks a block as a
# REFUSAL rather than a caller-error guard. A guard (`if (!r || !px || w <= 0)`)
# is a programming error and is not this gate's business; a failed create, a
# failed ensure, an over-capacity update and a missing mapping are refusals, and
# each one has to say so.
REFUSAL_COND = re.compile(
    r"![ \t]*createHost(Buffer|Image)\("
    r"|![ \t]*r->canvasMapped\["
    r"|size[ \t]*>[ \t]*m\.capacity"
    r"|![ \t]*mapped[ \t]*\)"
)
# `if (!ensureCanvasBuffer(...)) return;` is NOT in the list on purpose: that
# refusal is printed by `ensureCanvasBuffer` itself, which is the function that
# knows the size it asked for, and printing it again at the caller would say the
# same thing twice on the same frame.
BACKEND_SITES = [
    "mdk_render_create_index_texture",
    "mdk_render_create_rgba_texture",
    "mdk_render_create_palette",
    "mdk_render_update_texture",
    "mdk_render_create_mesh",
    "mdk_render_create_dynamic_mesh",
    "mdk_render_update_mesh",
    "ensureCanvasBuffer",
    "mdk_render_canvas_quad",
]
# The sites whose refusal prints a byte size, and the create call whose size
# argument that figure has to come from.
SIZE_SITES = ["mdk_render_create_mesh", "mdk_render_create_dynamic_mesh", "ensureCanvasBuffer"]

CHK = {
    "F1": "the object pass consults allocationFailed() and prints inside that test",
    "F2": "the refusal line prints the capacity that was refused (attemptedCapacity)",
    "F3": "the refusal line is edge-triggered and re-armed in the else sibling",
    "B1": "every backend refusal site prints before it returns",
    "B2": "the refusal helper formats the caller's varargs and flushes",
    "B3": "the printed byte figure is built from the size the create call was given",
    "B4": "every null-handle return in the file is preceded by a refusal",
    "B5": "every vkAllocate* result is read",
    "B6": "createHostImage does not claim success for an image it never filled",
    "B7": "a refusal prints once per distinct request, not once per refusal",
    "B8": "the one-shot command buffer and its pool are handed over together",
    "B9": "teardown frees the memory only after destroying what was bound to it",
}

# The identifiers a printed byte figure may use besides the ones in the create
# call's size argument: the frames-in-flight count and the namespace it lives in.
EXTRA_SIZE_TOKENS = {"kFramesInFlight", "mdk", "vk"}


def block_span_from(text, pos):
    """(open, close_after) of the first {...} block at or after pos."""
    i = text.find("{", pos)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return None


def body_span(text, name):
    """The span of a function definition's body, brace-matched from its name."""
    m = re.search(r"(?m)^[ \t]*[\w:<>,\* &]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*(?:const\s*)?\{",
                  text)
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def blocks(text, lo, hi):
    """Every {...} block inside text[lo:hi], as (cond_text, body, start)."""
    out = []
    for m in re.finditer(r"\b(if|else[ \t]+if)\s*\(", text[lo:hi]):
        start = lo + m.start()
        cond_end = close_brace(text, lo + m.end() - 1, "(", ")")
        if cond_end < 0:
            continue
        sp = block_span_from(text, cond_end)
        if sp is None:
            continue
        out.append((text[start:cond_end], text[sp[0]:sp[1]], sp))
    # A bare `} else {` is a refusal block too: `if (create) {...} else {refuse}`
    # is how the texture re-make writes it.
    for m in re.finditer(r"\}\s*else\s*\{", text[lo:hi]):
        start = lo + m.start()
        sp = block_span_from(text, start + 2)
        if sp is None:
            continue
        out.append(("else", text[sp[0]:sp[1]], sp))
    return out


def strip_comments(text):
    """Drop comment text: this tree documents its own rules in prose, and prose
    about `refused(` is not a call site.

    A line whose first non-space character is `*` is a block-comment continuation
    ONLY while a block comment is actually open. The old line-prefix test dropped
    them unconditionally, which silently deleted real statements — every
    `*outImage = VK_NULL_HANDLE;` in the file, and the `*outPool = pool;` that hands
    the one-shot pool back — from the text every check in this file reads. A gate
    that reads a doctored file is a gate that can fail on code that is present.
    """
    out = []
    in_block = False
    for line in text.splitlines(True):
        s = line.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("//"):
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block = True
            continue
        out.append(line)
    return "".join(out)


def close_brace(text, pos, open_ch, close_ch):
    """Index of the bracket matching the one at `pos`, skipping string literals: a
    refusal message contains parentheses, and a bracket inside a message is not a
    bracket in the code."""
    depth = 0
    quote = None
    k = pos
    while k < len(text):
        ch = text[k]
        if quote:
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return k
        k += 1
    return -1


def call_args(text, name):
    """The argument text of the first `name(` call in text, bracket-matched."""
    i = text.find(name + "(")
    if i < 0:
        return None
    j = text.find("(", i)
    end = close_brace(text, j, "(", ")")
    return text[j + 1:end] if end > 0 else None


def split_args(s):
    """Split an argument list at its top-level commas, ignoring commas inside
    nested brackets and inside string literals (a refusal message has both)."""
    args, depth, cur, quote = [], 0, "", None
    for ch in s:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
            cur += ch
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += ch
    args.append(cur)
    return args


def tokens(expr):
    return set(re.findall(r"[A-Za-z_]\w*", expr))


def enclosing_brace(text, pos):
    """Index of the `{` opening the innermost block that contains pos, or -1."""
    depth = 0
    for i in range(pos - 1, -1, -1):
        ch = text[i]
        if ch == "}":
            depth += 1
        elif ch == "{":
            if depth == 0:
                return i
            depth -= 1
    return -1


def open_brace_back(text, pos, open_ch, close_ch):
    """Index of the bracket matching the closer at `pos`, scanning backwards."""
    depth = 0
    k = pos
    while k >= 0:
        ch = text[k]
        if ch == close_ch:
            depth += 1
        elif ch == open_ch:
            depth -= 1
            if depth == 0:
                return k
        k -= 1
    return -1


def refusal_for_return(text, pos):
    """Is a `refused(` call attributable to THIS return, rather than merely
    somewhere in the same function?

    B4 used to ask whether `refused(` appeared anywhere between the innermost `{`
    and the return, and that is nearly right: inside a real `{...}` block, an
    earlier refusal in the function cannot vouch for the return. What it missed is
    the UNBRACED case — `if (cond) return VK_NULL_HANDLE;` has no block of its own,
    so its innermost `{` is the whole function body and any refusal the function
    already made vouched for a fresh silent guard, which review-6 F3 ran and found
    green. So the unbraced form is handled on its own terms: only its condition can
    carry the refusal, and a condition never does.

    A refusal followed by cleanup statements before the return still counts, which
    it must: `refused(...); vkDestroyCommandPool(...); return VK_NULL_HANDLE;` is
    how a refusal that has something to undo reads.
    """
    pre = text[:pos].rstrip()
    if pre.endswith(")"):
        open_ = open_brace_back(pre, len(pre) - 1, "(", ")")
        if open_ >= 0 and re.search(r"\bif$", pre[:open_].rstrip()):
            return "refused(" in pre[open_:]
    if re.search(r"\belse$", pre):
        return False
    open_ = enclosing_brace(text, pos)
    return open_ >= 0 and "refused(" in text[open_:pos]


def all_blocks(text):
    """Every {...} block in the text as (open, close_after)."""
    out = []
    for m in re.finditer(r"\{", text):
        sp = block_span_from(text, m.start())
        if sp:
            out.append(sp)
    return out


def line_of(text, pos):
    return text[:pos].count("\n") + 1


class Result:
    def __init__(self, who):
        self.who = who
        self.checks = 0
        self.failed = False
        self.broke = ""

    def check(self, label, ok, detail=""):
        self.checks += 1
        if ok:
            print("  ok   %s: %s" % (self.who, label))
        else:
            self.failed = True
            if not self.broke:
                self.broke = label
            print("  FAIL %s: %s%s" % (self.who, label, (" — " + detail) if detail else ""))

    def quiet(self, label, ok):
        self.checks += 1
        if not ok:
            self.failed = True
            if not self.broke:
                self.broke = label


def frontend_checks(r, text, quiet):
    code = strip_comments(text)

    def say(label, ok, detail=""):
        if quiet:
            r.quiet(label, ok)
        else:
            r.check(label, ok, detail)

    m = re.search(r"if[ \t]*\([ \t]*objectDraw\.allocationFailed\([ \t]*\)[ \t]*\)", code)
    sp = block_span_from(code, m.end() - 1) if m else None
    body = code[sp[0]:sp[1]] if sp else ""
    say(CHK["F1"], bool(m) and "printf(" in body,
        "a refused stream mesh draws no objects and says nothing, which is the silent "
        "missing picture object_draw.h records as owed to this frontend (review-11 F4)")

    # F2: the number printed must come from attemptedCapacity(). `capacity()` is 0
    # after a refusal (ensure releases the mesh before the request that fails), and
    # the frame's corner count is the pre-rounding figure.
    printed = re.findall(r"attemptedCapacity\(", body)
    wrong = re.findall(r"objectDraw\.capacity\(\)", body)
    say(CHK["F2"], bool(printed) and not wrong,
        "capacity() is exactly 0 after a refusal and the corner count is up to 8x "
        "smaller than the rounded-up request, so either one reports an allocation "
        "that was never attempted (review-11 F1)")

    # F3: the latch is declared, set inside the branch before the print, and
    # cleared in the else SIBLING of the allocationFailed test.
    declared = re.search(r"bool[ \t]+streamFailureLogged[ \t]*=[ \t]*false[ \t]*;", code)
    armed = re.search(r"if[ \t]*\([ \t]*![ \t]*streamFailureLogged[ \t]*\)", code)
    armed_before = False
    if armed and sp:
        am = re.search(r"streamFailureLogged[ \t]*=[ \t]*true[ \t]*;", code[sp[0]:sp[1]])
        pm = re.search(r"printf\(", code[sp[0]:sp[1]])
        armed_before = bool(am and pm and am.start() < pm.start())
    else_sib = re.search(
        r"if[ \t]*\([ \t]*objectDraw\.allocationFailed\([ \t]*\)[ \t]*\)"
        r".*?\}\s*else\s*\{\s*streamFailureLogged\s*=\s*false\s*;\s*\}",
        code, re.S)
    say(CHK["F3"], bool(declared) and bool(armed) and armed_before and bool(else_sib),
        "without the guard the refusal prints 60x/s; without the clear in the else "
        "sibling a recovered pass reports a refusal forever and a second, bigger "
        "refusal after a recovery is never printed (review-11 F7, review-12 R1)")


def backend_checks(r, text, quiet):
    code = strip_comments(text)

    def say(label, ok, detail=""):
        if quiet:
            r.quiet(label, ok)
        else:
            r.check(label, ok, detail)

    # B1: every refusal block in every named site carries a refused() call before
    # its return.
    missing = []
    found_any = False
    for name in BACKEND_SITES:
        sp = body_span(code, name)
        if sp is None:
            missing.append(name + " (function not found)")
            continue
        for cond, body, _ in blocks(code, sp[0], sp[1]):
            if not REFUSAL_COND.search(cond):
                continue
            found_any = True
            cut = body.find("refused(")
            ret = re.search(r"\breturn\b", body)
            if cut < 0 or (ret and ret.start() < cut):
                missing.append("%s: `%s ...` returns without refusing aloud" % (name, cond.strip()[:40]))
    say(CHK["B1"], found_any and not missing,
        "a refused resource is a missing picture with no line beside it; the Citro3D "
        "backend has always printed these and the Vulkan backend returned -1 in "
        "silence (review-11 F4)" + ("" if not missing else " — missing at " + "; ".join(missing)))

    # B2: the helper formats the caller's varargs AND flushes.
    sp = body_span(code, "refused")
    helper = code[sp[0]:sp[1]] if sp else ""
    say(CHK["B2"], "va_start(" in helper and ("vprintf(" in helper or "vsnprintf(" in helper)
        and "fflush(" in helper and "[vk] REFUSED" in code,
        "a refusal is the moment the process may die with the line that explains it "
        "still sitting in a stdio buffer")

    # B3: the byte figure printed IS the size expression the create call was given,
    # optionally times the frames-in-flight count that turns a per-slot request into
    # the whole one. The create call sits in the block's CONDITION (`if
    # (!createHostBuffer(&r->ctx, size, ...))`), so the condition and the body are
    # searched as one region. The size argument is read as an EXPRESSION rather than a
    # bare identifier, and EVERY size site has to contribute: `checked > 0` let one
    # readable site vouch for all of them, so a refactor to `sizeof(mdk::Vertex) * n`
    # would have dropped a site from the comparison without a sound (review-5 F4).
    #
    # THE RULE IS TWO-SIDED NOW, and it was not. The first version only rejected
    # FOREIGN tokens (`tokens(figure) - want - EXTRA`), so any figure that mentioned
    # LESS than the size passed: `kFramesInFlight` on its own, or `cap / 8`, or
    # `cap * 4` — all subsets of what was allowed. Both were run against that gate
    # and both came back green, which is the review-5 F3 defect (a printed figure
    # that disagrees with the request) walking straight back in through the check
    # built to stop it (review-6 F1). So the figure's identifiers must be exactly the
    # size's, or the size's plus the frames-in-flight count, AND the figure may not
    # introduce an operator or a numeric literal the size expression did not have.
    # What that pins is identifiers and the absence of a rescale, not arithmetic: a
    # figure of `size + size` would still pass, and saying so here is better than
    # claiming a shape gate can evaluate C++.
    bad = []
    checked = 0
    per_site = {}
    for name in SIZE_SITES:
        sp = body_span(code, name)
        if sp is None:
            bad.append(name + " (not found)")
            continue
        for cond, block, _ in blocks(code, sp[0], sp[1]):
            if not REFUSAL_COND.search(cond):
                continue
            region = cond + block
            cargs = call_args(region, "createHostBuffer")
            if cargs is None:
                cargs = call_args(region, "createHostImage")
            call = call_args(block, "refused")
            if cargs is None or call is None:
                continue
            args = split_args(cargs)
            if len(args) < 2 or not tokens(args[1]):
                continue
            want = tokens(args[1])
            figures = [a for a in split_args(call) if "unsigned long long" in a]
            if not figures:
                continue
            checked += 1
            per_site[name] = per_site.get(name, 0) + 1
            src_ops = set(re.findall(r"[/*%]", args[1]))
            src_nums = set(re.findall(r"\d+", args[1]))
            request = " ".join(args[1].split())
            for f in figures:
                expr = re.sub(r"\(\s*unsigned long long\s*\)", " ", f)
                # The frames-in-flight count is the one extra factor that belongs in
                # a request: the create call is per slot and the line prints both the
                # per-slot figure and the total.
                flat = re.sub(r"\*?\s*(?:mdk::vk::)?kFramesInFlight\s*\*?", " ", expr)
                toks = tokens(flat)
                if not (want <= toks and toks <= want | EXTRA_SIZE_TOKENS):
                    bad.append("%s prints a figure built from `%s`, not from the `%s` it "
                               "handed the create call"
                               % (name, " ".join(sorted(toks)) or "nothing at all",
                                  request))
                elif (set(re.findall(r"[/*%]", flat)) - src_ops
                      or set(re.findall(r"\d+", flat)) - src_nums):
                    bad.append("%s rescales its figure: it prints `%s` for a request of "
                               "`%s`, so the bytes in the log disagree with what the "
                               "driver was asked for by that factor"
                               % (name, " ".join(flat.split()), request))
    unread = [n for n in SIZE_SITES if n not in per_site]
    say(CHK["B3"], checked > 0 and not bad and not unread,
        "two numbers for one allocation is the review-11 F1 defect: a printed figure "
        "derived from something else disagrees with the request by up to 8x" +
        ("" if not bad else " — " + "; ".join(bad)) +
        ("" if checked else " — and no size site was readable at all, so this check "
                            "compared nothing") +
        ("" if not unread else " — and %s could not be read, so nothing there was "
                               "compared" % ", ".join(unread)))

    # B4: the DISCOVERY rule. B1 can only check the sites it was handed, and
    # review-5 F1 was a site it had not been handed: `getDescriptorSet` returned
    # VK_NULL_HANDLE in silence and its callers drew on with the PREVIOUS draw's set
    # bound. This one reads the whole file, so a null handle cannot be quiet.
    #
    # The refusal is attributed to the RETURN, not to the function. The first version
    # asked whether `refused(` appeared anywhere between the innermost `{` and the
    # return, and an unbraced `if (cond) return VK_NULL_HANDLE;` has the whole
    # function body as its innermost block — so a new silent guard in a function that
    # already refused once was vouched for by a refusal that has nothing to do with
    # it, which review-6 F3 ran and found green.
    silent = []
    nulls = list(re.finditer(r"\breturn[ \t]+VK_NULL_HANDLE[ \t]*;", code))
    for m in nulls:
        if not refusal_for_return(code, m.start()):
            silent.append("line %d" % line_of(code, m.start()))
    say(CHK["B4"], bool(nulls) and not silent,
        "a null handle is a resource the caller does not have; a file that returns one "
        "without saying so is the silent missing picture this gate exists to stop" +
        ("" if not silent else " — silent at " + ", ".join(silent)))

    # B5: an unchecked VkResult is an undefined out-parameter. The one review-5
    # found was `vkAllocateCommandBuffers`, whose handle the very next call began.
    unchecked = []
    allocs = list(re.finditer(r"\bvkAllocate[A-Za-z]*\s*\(", code))
    for m in allocs:
        start = -1
        for ch in (";", "{", "}"):
            start = max(start, code.rfind(ch, 0, m.start()))
        end = code.find(";", m.end())
        stmt = code[start + 1:end if end > 0 else len(code)]
        if "VK_SUCCESS" not in stmt:
            unchecked.append("line %d" % line_of(code, m.start()))
    say(CHK["B5"], bool(allocs) and not unchecked,
        "a discarded VkResult leaves the handle undefined and the next call uses it" +
        ("" if not unchecked else " — unchecked at " + ", ".join(unchecked)))

    # B6: the one refusal that used to report SUCCESS. `createHostImage` made the
    # image, failed to get a command buffer to fill and transition it, and returned
    # true: a texture of undefined bytes with no failure anywhere (review-5 F1).
    filled = False
    sp = body_span(code, "createHostImage")
    if sp:
        body = code[sp[0]:sp[1]]
        seg = ""
        m = re.search(r"\bif[ \t]*\([ \t]*![ \t]*cb[ \t]*\)", body)
        if m:
            sp2 = block_span_from(body, m.end() - 1)
            seg = body[sp2[0]:sp2[1]] if sp2 else ""
        else:
            m = re.search(r"\bif[ \t]*\([ \t]*cb[ \t]*\)", body)
            if m:
                sp2 = block_span_from(body, m.end() - 1)
                if sp2:
                    em = re.match(r"\s*else\s*\{", body[sp2[1]:])
                    if em:
                        sp3 = block_span_from(body, sp2[1] + em.end() - 1)
                        seg = body[sp3[0]:sp3[1]] if sp3 else ""
        filled = "refused(" in seg and "return false" in seg
    say(CHK["B6"], filled,
        "an image that was never filled or transitioned is not a texture, and returning "
        "true for it is a refusal the log never sees (review-5 F1)")

    # B7: one line per DISTINCT request. The Citro3D backend latches with a bool
    # because its mesh line printed 1710 times in one run (review-10 F3); a bool is
    # too coarse here, where a descriptor set is allocated per draw, so the helper
    # compares the rendered line and stays silent only for a request it has already
    # named (review-5 F2).
    cmp_ = re.search(r"strcmp\(", helper)
    early = re.search(r"\breturn[ \t]*;", helper)
    scan = re.search(r"for[ \t]*\([^)]*size\(\)", helper)
    printed = helper.find("[vk] REFUSED")
    say(CHK["B7"],
        bool(cmp_ and early and scan and printed > 0 and cmp_.start() < early.start() < printed),
        "a per-draw refusal that prints every time is the 1710-lines-in-one-run flood "
        "the console already gated against (review-5 F2, review-10 F3) — and a latch "
        "that remembers only the LAST request re-prints a rotating set of them every "
        "frame, which measured 22500 lines for 48 distinct requests")

    # B8: the one-shot command buffer and the pool it came from travel together, and
    # every failure exit after the pool exists destroys the pool it made. They used
    # to be paired by POSITION in a file-scope vector whose front `endOneShot` popped,
    # which was sound only while no refusal landed between the push and the return.
    # One stranded entry and every later `endOneShot` freed a foreign command buffer
    # into a pool it had not come from and destroyed that pool a step early —
    # `vkFreeCommandBuffers` requires the pool the handle was allocated from
    # (review-6 F2). A pool the caller holds cannot be stranded, so that is the shape
    # pinned here.
    sp = body_span(code, "beginOneShotTracked")
    obody = code[sp[0]:sp[1]] if sp else ""
    esp = body_span(code, "endOneShot")
    ebody = code[esp[0]:esp[1]] if esp else ""
    stranded = []
    if sp:
        made = -1
        m0 = re.search(r"if[ \t]*\([ \t]*vkCreateCommandPool", obody)
        if m0:
            sp2 = block_span_from(obody, obody.find("{", m0.end() - 1))
            made = sp2[1] if sp2 else -1
        for m in re.finditer(r"\breturn[ \t]+VK_NULL_HANDLE[ \t]*;", obody):
            if m.start() > made:
                open_ = enclosing_brace(obody, m.start())
                if "vkDestroyCommandPool" not in obody[open_:m.start()]:
                    stranded.append("line %d" % line_of(code, sp[0] + m.start()))
    say(CHK["B8"], bool(sp) and bool(esp)
        and "*outPool = pool" in obody
        and "vkFreeCommandBuffers(ctx->device, pool" in ebody
        and "vkDestroyCommandPool(ctx->device, pool" in ebody
        and "[0]" not in ebody
        and not stranded,
        "a command buffer freed into a pool it did not come from, and that pool "
        "destroyed under it, is corruption rather than a missing picture; pairing "
        "them by position let one refusal desynchronize the pairing for the rest of "
        "the run" + ("" if not stranded else " — pool stranded at " + ", ".join(stranded)))

    # B9: in a teardown block the memory is released LAST, after the buffer or image
    # bound to it is destroyed. `vkFreeMemory` on memory still bound to a live object
    # is invalid use, and this file had two of them: `createHostImage`'s staging
    # failure path (review-6 F6) and `createHostBuffer`'s map failure path, found
    # while fixing the first.
    #
    # The rule is a shape and it is deliberately coarse: in one block that both
    # destroys a buffer/image and frees memory, the LAST such call directly inside it
    # must be the free. Every correct teardown in the file has that shape, and it is
    # what goes red when a free is moved above the destroy it belongs to. It does NOT
    # prove the free releases THAT object's own memory — a block that frees A after
    # destroying B passes — and it says nothing across blocks.
    bad9 = []
    spans = all_blocks(code)
    for lo, hi in spans:
        inner = [(a, b) for a, b in spans if a > lo and b <= hi]
        calls = []
        for m in re.finditer(r"vkFreeMemory\s*\(|vkDestroy(?:Image|Buffer)\s*\(", code[lo:hi]):
            p = lo + m.start()
            if any(a <= p < b for a, b in inner):
                continue
            calls.append((p, m.group(0)))
        if not any("vkFreeMemory" in c for _, c in calls):
            continue
        if not any("vkDestroy" in c for _, c in calls):
            continue
        if "vkFreeMemory" not in max(calls)[1]:
            bad9.append("line %d" % line_of(code, max(calls)[0]))
    say(CHK["B9"], bool(spans) and not bad9,
        "memory still bound to a live image or buffer must not be freed; the destroy "
        "comes first" + ("" if not bad9 else " — the last teardown call is a free at "
                         + ", ".join(bad9)))


def adjudicate(who, text, quiet=False):
    r = Result(who)
    if who.startswith("Linux frontend"):
        frontend_checks(r, text, quiet)
    else:
        backend_checks(r, text, quiet)
    return r


FILES = {}
for who, rel in (("Linux frontend", FRONTEND), ("Linux backend", BACKEND)):
    try:
        FILES[who] = open(ROOT + "/" + rel, errors="replace").read()
    except OSError as exc:
        print("  FAIL linux_refusal_report: cannot read %s (%s)" % (rel, exc))
        sys.exit(1)

real = [adjudicate("Linux frontend", FILES["Linux frontend"]),
        adjudicate("Linux backend", FILES["Linux backend"])]

# ---------------------------------------------------------------------------
# The arms. Each is a realistic edit that COMPILES — a mutant that would not build
# is a mutant of nothing (review-15 F3) — and each names the one check that must
# catch it.
# ---------------------------------------------------------------------------
FRONT_ARMS = []
BACK_ARMS = []


def arm_frontend(label, mutate, expected):
    FRONT_ARMS.append((label, mutate, CHK[expected]))


def arm_backend(label, mutate, expected):
    BACK_ARMS.append((label, mutate, CHK[expected]))


def fe_delete_refusal_block(text):
    """The batch undone: the whole allocationFailed block goes, and the pass is
    silent again. Only F1 can catch it."""
    m = re.search(r"if[ \t]*\([ \t]*objectDraw\.allocationFailed\([ \t]*\)[ \t]*\)", text)
    if not m:
        return text
    sp = block_span_from(text, m.end() - 1)
    if sp is None:
        return text
    end = sp[1]
    # The `else { streamFailureLogged = false; }` sibling goes with it.
    tail = re.match(r"\s*else\s*\{", text[end:])
    if tail:
        sp2 = block_span_from(text, end + tail.end() - 1)
        if sp2:
            end = sp2[1]
    start = text.rfind("\n", 0, m.start()) + 1
    return text[:start] + text[end:]


def fe_prints_capacity_not_attempted(text):
    """The tidy-up that reads capacity() — which is 0 after a refusal. F2 only."""
    out = text.replace("objectDraw.attemptedCapacity()", "objectDraw.capacity()")
    return out if out != text else text


def fe_latch_never_clears(text):
    """The clear deleted with its `else` sibling: the latch arms once for the run,
    so a second, bigger refusal after a recovery is silent. F3 only."""
    out = re.sub(r"\}\s*else\s*\{\s*streamFailureLogged\s*=\s*false\s*;\s*\}", "}", text, count=1)
    return out if out != text else text


def fe_latch_guard_widened(text):
    """`if (!streamFailureLogged)` -> `if (true)`: the line prints every frame of a
    refusal, and the assignment line the weaker check read is still there. F3 only."""
    out = re.sub(r"if[ \t]*\([ \t]*![ \t]*streamFailureLogged[ \t]*\)", "if (true)", text, count=1)
    return out if out != text else text


def be_delete_dynamic_mesh_refusal(text):
    """One site loses its print. B1 only."""
    m = re.search(r"[ \t]*refused\(\"dynamic mesh[^;]*;\n", text, re.S)
    return text[:m.start()] + text[m.end():] if m else text


def be_helper_stops_flushing(text):
    """The flush dropped: the line is produced and buffered. B2 only."""
    out = text.replace('std::printf("[vk] REFUSED %s\\n", text);\n    std::fflush(stdout);',
                       'std::printf("[vk] REFUSED %s\\n", text);', 1)
    return out if out != text else text


def be_delete_descriptor_refusal(text):
    """The site the list never named (review-5 F1): the print goes, and a gate that
    only checks a list of functions does not notice. B4 only."""
    m = re.search(r"[ \t]*refused\(\"descriptor set[^;]*;\n", text, re.S)
    return text[:m.start()] + text[m.end():] if m else text


def be_command_buffer_result_dropped(text):
    """The unchecked VkResult review-5 found, restored: the allocate's result is
    discarded again and the handle the next call begins with is undefined. B5 only."""
    i = text.find("if (vkAllocateCommandBuffers")
    if i < 0:
        return text
    j = text.find("\n    }\n", i)
    if j < 0:
        return text
    return (text[:i] + "vkAllocateCommandBuffers(ctx->device, &cbAlloc, &cb);"
            + text[j + 7:])


def be_image_reports_success(text):
    """The `return false` that review-5 F1 was, turned back into `return true`: the
    image exists but was never filled or transitioned, and the function still
    claims it is a texture. B6 only."""
    return text.replace("        *outMemory = VK_NULL_HANDLE;\n        return false;\n    }\n"
                        "    if (staging) {",
                        "        *outMemory = VK_NULL_HANDLE;\n        return true;\n    }\n"
                        "    if (staging) {", 1)


def be_helper_prints_every_refusal(text):
    """The dedup deleted: a sustained refusal prints again on every frame, which is
    the 1710-lines-in-one-run flood review-10 F3 gated on the console. B7 only."""
    return text.replace("""        if (std::strcmp(g_refusalLines[i].text, text) == 0) {
            return;  // this exact request was named already
        }
""", "", 1)


def be_helper_remembers_only_the_last(text):
    """The one-slot latch: it compares against a remembered line, so a shape check
    passes, but it remembers only the first entry — a site that refuses 48
    different requests in a frame then reprints all 48 on every frame after it.
    B7 only."""
    return text.replace("for (size_t i = 0; i < g_refusalLines.size(); i++) {",
                        "for (size_t i = 0; i < 1; i++) {", 1)


def be_composite_size_prints_capacity(text):
    """The refactor review-5 F4 described: the create call is handed a composite
    expression and the line prints the mesh's own capacity field. The old B3 read
    only a bare identifier, stopped checking this site, and stayed green. B3 only."""
    out = text.replace(
        "if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "
        "&m.buffer[0],",
        "if (!createHostBuffer(&r->ctx, sizeof(mdk::Vertex) * (VkDeviceSize)n, "
        "VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer[0],", 1)
    if out == text:
        return text
    return out.replace("(unsigned long long)size);\n        m.used = false;",
                       "(unsigned long long)m.capacity);\n        m.used = false;", 1)


def be_prints_the_old_capacity(text):
    """The refusal prints the mesh's own capacity field instead of the `size` it
    handed the create call. B3 only."""
    out = text.replace("(unsigned long long)size);\n        m.used = false;",
                       "(unsigned long long)m.capacity);\n        m.used = false;", 1)
    return out if out != text else text


def be_figure_prints_frames_only(text):
    """The figure stops naming the size at all and prints the frames-in-flight count
    instead. The old one-sided rule only rejected FOREIGN tokens, so a figure that
    mentioned LESS than the size was a subset of what it allowed and slipped past
    (review-6 F1). B3 only."""
    return text.replace("(unsigned long long)(cap * mdk::vk::kFramesInFlight),",
                        "(unsigned long long)mdk::vk::kFramesInFlight,", 1)


def be_figure_rescaled(text):
    """The total is mis-scaled: `cap / 8` where the request is `cap` times the frames
    in flight, and the per-slot figure inflated by 4. That is review-5 F3's defect
    exactly, and the old rule passed it because `cap` was still in the figure
    (review-6 F1). B3 only."""
    out = text.replace("(unsigned long long)(cap * mdk::vk::kFramesInFlight),",
                       "(unsigned long long)(cap / 8),", 1)
    if out == text:
        return text
    return out.replace("(unsigned long long)cap, (unsigned)mdk::vk::kFramesInFlight);",
                       "(unsigned long long)(cap * 4), (unsigned)mdk::vk::kFramesInFlight);", 1)


def be_unbraced_silent_guard(text):
    """A new bounds guard that returns a null handle with no refusal of its own, in
    the unbraced single-statement style this file uses. The function already refuses
    once a few lines earlier, so a rule that attributes a refusal to the FUNCTION
    rather than to the return lets it through (review-6 F3). B4 only."""
    return text.replace("    VkDescriptorImageInfo texInfo = {};",
                        "    if (texture >= (int32_t)r->textures.size()) return VK_NULL_HANDLE;\n"
                        "    VkDescriptorImageInfo texInfo = {};", 1)


def be_oneshot_pool_not_handed_over(text):
    """The caller gets a command buffer and no pool, so the pairing is somebody
    else's problem again. B8 only."""
    return text.replace("    *outPool = pool;\n    return cb;", "    return cb;", 1)


def be_oneshot_pool_stranded(text):
    """The begin-failure exit returns without destroying the pool it made. One
    stranded entry desynchronizes every later `endOneShot` (review-6 F2). B8 only."""
    return text.replace("""                "nowhere to be recorded");
        vkFreeCommandBuffers(ctx->device, pool, 1, &cb);
        vkDestroyCommandPool(ctx->device, pool, nullptr);
        return VK_NULL_HANDLE;""",
                        """                "nowhere to be recorded");
        return VK_NULL_HANDLE;""", 1)


def be_oneshot_positional_ledger(text):
    """The old pairing restored: `endOneShot` finds the pool by POSITION in a
    file-scope vector instead of being handed it, so one refusal desynchronizes it
    for the rest of the run (review-6 F2). B8 only."""
    out = text.replace(
        "void endOneShot(mdk::vk::Context* ctx, VkCommandPool pool, VkCommandBuffer cb) {",
        "std::vector<VkCommandPool> g_oneShotPools;\n\n"
        "void endOneShot(mdk::vk::Context* ctx, VkCommandPool pool, VkCommandBuffer cb) {", 1)
    if out == text:
        return text
    return out.replace("    vkFreeCommandBuffers(ctx->device, pool, 1, &cb);",
                       "    vkFreeCommandBuffers(ctx->device, g_oneShotPools[0], 1, &cb);", 1)


def be_free_before_destroy(text):
    """The order review-6 F6 found, restored: the memory bound to a live image is
    released BEFORE the image is destroyed, which is invalid use. B9 only."""
    return text.replace("""        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (pixels) {""",
                        """        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }
    if (pixels) {""", 1)


arm_frontend("refusal-block-deleted", fe_delete_refusal_block, "F1")
arm_frontend("prints-capacity-not-attempted-capacity", fe_prints_capacity_not_attempted, "F2")
arm_frontend("latch-clear-deleted", fe_latch_never_clears, "F3")
arm_frontend("latch-guard-widened-to-true", fe_latch_guard_widened, "F3")
arm_backend("site-refusal-print-deleted", be_delete_dynamic_mesh_refusal, "B1")
arm_backend("helper-stops-flushing", be_helper_stops_flushing, "B2")
arm_backend("prints-the-mesh-capacity", be_prints_the_old_capacity, "B3")
arm_backend("composite-size-prints-capacity", be_composite_size_prints_capacity, "B3")
arm_backend("figure-prints-frames-in-flight-only", be_figure_prints_frames_only, "B3")
arm_backend("figure-rescaled-by-a-factor", be_figure_rescaled, "B3")
arm_backend("descriptor-refusal-deleted", be_delete_descriptor_refusal, "B4")
arm_backend("unbraced-silent-null-handle-guard", be_unbraced_silent_guard, "B4")
arm_backend("command-buffer-result-dropped", be_command_buffer_result_dropped, "B5")
arm_backend("unfilled-image-reports-success", be_image_reports_success, "B6")
arm_backend("helper-prints-every-refusal", be_helper_prints_every_refusal, "B7")
arm_backend("helper-remembers-only-the-last", be_helper_remembers_only_the_last, "B7")
arm_backend("oneshot-pool-not-handed-over", be_oneshot_pool_not_handed_over, "B8")
arm_backend("oneshot-pool-stranded-on-refusal", be_oneshot_pool_stranded, "B8")
arm_backend("oneshot-pairing-by-position-restored", be_oneshot_positional_ledger, "B8")
arm_backend("memory-freed-before-destroy", be_free_before_destroy, "B9")

checks = sum(r.checks for r in real)
arms = [("Linux frontend", FRONT_ARMS, FILES["Linux frontend"]),
        ("Linux backend", BACK_ARMS, FILES["Linux backend"])]
total = sum(len(a[1]) for a in arms)
print("linux_refusal_report: %d checks, %d mutation arms" % (checks, total))
arms_red = 0
for who, table, source in arms:
    for label, mutate, expected in table:
        mutated = mutate(source)
        if mutated == source:
            print("  FAIL %s arm %s: the mutation changed nothing, so the arm tested the "
                  "file it was handed instead of the invariant" % (who, label))
            continue
        after = adjudicate(who + " arm", mutated, quiet=True)
        if not after.failed:
            print("  FAIL %s arm %s: the mutation was accepted, so the check does not pin "
                  "what it claims to pin" % (who, label))
            continue
        if after.broke != expected:
            print("  FAIL %s arm %s: it turned `%s` red, but the check it claims to pin is "
                  "`%s` — so that check has no mutation that isolates it" %
                  (who, label, after.broke, expected))
            continue
        arms_red += 1
        print("  ok   %s arm %s: turned `%s` red as required" % (who, label, after.broke))

if any(r.failed for r in real):
    sys.exit(1)
if arms_red != total:
    print("  FAIL linux_refusal_report: only %d of %d arms adjudicated red; a PASS here "
          "would be a reading of nothing" % (arms_red, total))
    sys.exit(1)
print("PASS LinuxRefusalReport (%d checks, %d arms adjudicated red)" % (checks, arms_red))
PY
exit $?
