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
#        the others (review-5 F4, review-6 F1). And EVERY number in the line counts,
#        including the frames-in-flight count itself, which a reader of only the
#        `unsigned long long` arguments never saw (review-7 F3);
#     B4 DISCOVERY, not a list: every null-handle return in the file has a `refused(`
#        attributed to THAT return. B1 can only check the sites it was told about, and
#        review-5 F1 was a site it had not been told about (`getDescriptorSet`, whose
#        callers then drew with the PREVIOUS draw's descriptor set bound — a wrong
#        picture, not a missing one). Attribution is per return and not per function,
#        because an unbraced `if (x) return VK_NULL_HANDLE;` in a function that already
#        refuses once is otherwise vouched for by a refusal that has nothing to do with
#        it (review-6 F3). And the discovery is not spelling-dependent: a rule that
#        matches one spelling of a null return is a rule that can be satisfied by
#        retyping the return (review-7 F4);
#     B5 every result-bearing Vulkan call has its VkResult read. An unchecked
#        `vkAllocateCommandBuffers` hands back an undefined handle that the next call
#        happily dereferences (review-5 F1), and so does the `vkQueueSubmit` whose
#        refusal nobody read (review-7 F1) — so the obligation follows the spec's
#        naming convention, not the one verb family an earlier version named. The void
#        entry points are NAMED one by one rather than excluded by verb, because
#        `vkGet*` is not a void family (review-8 F3);
#     B6 `createHostImage` does not report success for an image whose fill and layout
#        transition were never executed, whether that is because there was no command
#        buffer or because its submit was refused (review-5 F1, review-7 F1). The
#        refusal's CONDITION is pinned too: a block that refuses and returns false for
#        the wrong reason is not a guard (review-8 F2);
#     B7 a refusal prints ONCE PER DISTINCT REQUEST. The Citro3D backend latches
#        with a bool and it must, because that line printed 1710 times in one run
#        (review-10 F3); a bool is too coarse here, where a descriptor set is
#        allocated per DRAW. So the helper keeps the SET of requests each site has
#        named and is silent only for one of them — and it has to be a set: a latch
#        that remembers only the LAST request reprints a rotating set of them on
#        every frame, which measured 22500 lines for 48 distinct requests
#        (review-5 F2).
#     B8 the one-shot command buffer and the pool it came from are handed over
#        together — AT THE CALL SITE, not only inside the two functions — and every
#        failure exit after the pool exists destroys the pool it made. They used to be
#        paired by POSITION in a file-scope vector, and one stranded entry made every
#        later `endOneShot` free a foreign command buffer into the wrong pool
#        (review-6 F2, review-7 F2). And nothing may return between the begin and the
#        end except the exit the callee already cleaned up after (review-8 F4);
#     B9 in a teardown block the memory is released LAST, after the buffer or image
#        bound to it is destroyed. `vkFreeMemory` on memory still bound to a live
#        object is invalid use, and this file had two of them (review-6 F6).
#     B10 a one-shot submission the wait could not prove finished releases NOTHING it
#        named — no free, no destroy, no unmap. A failed wait is a completion boundary
#        that was not established, not a completion, and AGENTS.md P3 puts the boundary
#        before CPU reuse (review-8 F1). It covers the CALLEE too: `endOneShot` owns the
#        command buffer and the pool the submission is made of, and freeing those under a
#        submission that may still be running is the same defect one level down
#        (review-1 F3).
#     B11 the FRAME SEAM reads both of its results. `beginFrame` and `endFrame` each
#        return bool, and until this batch the seam returned silently on the first and
#        discarded the second outright: a frame whose fence wait, image acquire,
#        command-buffer reset, begin, submit or present failed was reported as a
#        rendered frame, and its screenshot was captured and published as the pinned
#        record of a frame that never reached the display.
#     B12 a descriptor set the cache forgets goes BACK to the pool, and not before the
#        frame that named it has finished. `forgetDescriptorSets` was review-25's fix
#        for a cached set naming a destroyed image view, and it is correct — but a cache
#        entry and an allocated set are two things and only one of them was released.
#        With no `vkFreeDescriptorSets` and no `vkResetDescriptorPool` anywhere, a pool
#        built on a fixed `maxSets` spent a set on every texture or palette released and
#        re-made for the life of the process, and when it emptied the B4 refusal started
#        skipping draws. Three shapes, because there are three distinct ways to be
#        wrong: the pool must ask for freeable sets, the drop site must retire rather
#        than free, and the settle must actually free on the buffers' own deadline.
#        And the JOIN is pinned, not just the parts (review-9 F1): the retire must
#        record what it was handed, the drop site must hand over the set of the entry
#        it is dropping, the deadline must be the boundary with no offset on it, and
#        the settle must remove the entry it freed. Four mutants that restored the
#        leak, double-freed a live set, or freed one frame early all printed PASS
#        while the three parts each stayed individually green.
#
#   context (mdk_linux/src/render/vk/vk_context.cpp)
#     C1 every result-bearing Vulkan call in the file has its result read. This is the
#        property B5 was written for, one file over, and the file is where review-8 F3
#        said it still lived: 24 of the 39 result-bearing calls there discarded their
#        VkResult, the worst being a failed surface-capabilities query that left a
#        zero-initialised extent and made vkCreateSwapchainKHR fail with a line
#        blaming the create. The rule is B5's plus the file's own idiom (see
#        `unchecked_result_calls`), because this file reads a result by naming it and
#        testing it on the next line.
#     C2 the FRAME's command buffer is ENDED before it is submitted. Only a pending or
#        executable command buffer may be named by VkSubmitInfo::pCommandBuffers, and the
#        frame path submitted a buffer that was begun and never ended: an unterminated
#        stream that lavapipe happens to execute, which is why 138 committed frames said
#        nothing about it (review-1 F1).
#     C3 a refused frame never leaves its slot's fence UNSIGNALLED. A wait does not reset
#        a fence, so `beginFrame` must not reset one at all — every refusal there leaves
#        the fence signalled and the next wait returns at once — and the one place that
#        does reset one, `endFrame` before its submit, has to put a signalled fence back
#        when the submit is the thing that failed (review-1 F2's premise, checked rather
#        than argued).
#     C4 a frame that acquired an image and then refused CONSUMES the acquire's signal.
#        `presentWait` is a BINARY semaphore: a posted one that nobody waits on makes the
#        next acquire invalid use, so every refusal after a successful acquire has to
#        drain it (review-1 F4).
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
CONTEXT = "mdk_linux/src/render/vk/vk_context.cpp"

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
    "B5": "every result-bearing Vulkan call's VkResult is read",
    "B6": "createHostImage does not claim success for an image it never filled",
    "B7": "a refusal prints once per distinct request, not once per refusal",
    "B8": "the one-shot command buffer and its pool are handed over together",
    "B9": "teardown frees the memory only after destroying what was bound to it",
    "B10": "a one-shot submission whose completion was never proven releases nothing",
    "B11": "the frame seam reads both beginFrame's and endFrame's result and says so",
    "B12": "a descriptor set the cache forgets returns to the pool, after its frame "
           "has finished, with its result read, and the retire/settle join is what "
           "makes that true",
    "C1": "every result-bearing Vulkan call in vk_context.cpp has its result read",
    "C2": "the frame's command buffer is ended before it is submitted",
    "C3": "a refused frame never leaves its slot's fence unsignalled",
    "C4": "a frame that acquired an image and refused consumes the acquire's signal",
}

# The identifiers a printed byte figure may use besides the ones in the create
# call's size argument: the frames-in-flight count and the namespace it lives in.
EXTRA_SIZE_TOKENS = {"kFramesInFlight", "mdk", "vk"}

# A printed figure is any refused() argument carrying a cast to an integral type.
# Keying on the two-word `unsigned long long` spelling alone left the frames-in-flight
# count — printed as `(unsigned)mdk::vk::kFramesInFlight` — derived by nothing, while
# the check's own comment called it the one extra factor that belongs (review-7 F3).
INTEGRAL_CAST = re.compile(
    r"\(\s*(?:unsigned long long|unsigned short|unsigned int|unsigned|signed|long"
    r"|short|int|size_t|VkDeviceSize|uint32_t|int32_t)\s*\)")

# The Vulkan entry-point families whose calls return a VkResult, by the spec's naming
# convention. This is hardcoded rather than parsed out of vulkan_core.h because a gate
# that stops checking when its input is missing is a pass generator, and because the
# point is to cover the NEXT result-bearing call in a checked family, not only the ones
# that were missed once.
#
# The exclusion used to be described as "the void families ... vkDestroy*, vkFree*,
# vkUnmap*, vkCmd*, vkGet* and vkUpdate*", and that was simply false: `vkGet*` is NOT a
# void family. `vkGetSwapchainImagesKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`,
# `vkGetPhysicalDeviceSurfaceSupportKHR`, `vkGetPhysicalDeviceSurfaceFormatsKHR`,
# `vkGetPhysicalDeviceSurfacePresentModesKHR` and `vkGetFenceStatus` all return a
# VkResult, and the file that excludes the whole family by verb cannot see one of them
# being discarded (review-8 F3). What is true is narrower and has to be a list: these
# are the VOID entry points this file calls, named one at a time. Any other `vkGet*` is
# treated as result-bearing, which is the family's default.
RESULT_VERBS = ("Create", "Allocate", "Bind", "Map", "Begin", "End", "Queue",
                "Device", "Wait", "Reset", "Acquire", "Enumerate", "Flush",
                "Store", "Invalidate", "Release")
VOID_ENTRY_POINTS = {
    "vkGetPhysicalDeviceMemoryProperties",
    "vkGetBufferMemoryRequirements",
    "vkGetImageMemoryRequirements",
}

# The context file calls three more entry points that are not result-bearing:
# `vkGetPhysicalDeviceQueueFamilyProperties` and `vkGetDeviceQueue` are void, and
# `vkGetInstanceProcAddr` returns a function POINTER, not a VkResult. They are
# added here and NOT to VOID_ENTRY_POINTS above, because that set is B5's and B5
# must not move while a new check is being introduced. The addition is provably
# neutral for B5: `renderer_vk.cpp` calls none of the three (grep counts zero for
# each), so no verdict B5 gives can change because of this list.
CONTEXT_VOID_ENTRY_POINTS = VOID_ENTRY_POINTS | {
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetDeviceQueue",
    "vkGetInstanceProcAddr",
}


def strip_code(text):
    """Blank out comments AND string literals, keeping every line number.

    String literals matter here in a way they did not for the other checks: this
    file's refusal messages NAME the entry point that failed —
    `vkFailed("vkBindImageMemory(depth)", result)` — and a scan that reads a
    message sees a call site that does not exist. Without this, the pre-batch file
    scored 60 discarded results where it has 23, and every one of the extra 37 was
    a string.

    Newlines are preserved so the line numbers this gate prints are the file's
    (review-8 F5).
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if text[i:i + 2] == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if text[i:i + 2] == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
            continue
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch:
                if text[j] == "\\":
                    j += 1
                j += 1
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def unchecked_result_calls(code, void_entry_points):
    """Every result-bearing Vulkan call whose VkResult is not read, as (line, name).

    A result is READ in one of two shapes, and both are accepted because the two
    files in this gate use different ones:

      1. the call sits inside the test itself — `if (vkCreateBuffer(...) !=
         VK_SUCCESS)` — which is `renderer_vk.cpp`'s house style and is all B5
         accepts;
      2. the call is assigned to a name and the next thing that touches that name
         compares it against a `VK_` constant — `VkResult result = vkResetFences(...);
         if (result != VK_SUCCESS)`. This is `vk_context.cpp`'s house style, and it
         is the shape the two-call enumeration idiom needs, because a fill call may
         legitimately answer VK_INCOMPLETE and a statement-local test cannot say so.

    A reassignment before the first comparison is RED, not read: a value overwritten
    before it was examined was never examined.

    What this still cannot see: that the test does the right thing with the value, or
    that the branch it guards is reachable. It is a shape rule, like every other
    check in this file, because the shape is what a driver refusing is invisible
    about.
    """
    calls = [m for m in re.finditer(r"\bvk(?:%s)[A-Za-z0-9_]*" % "|".join(RESULT_VERBS), code)
             if code[m.end():].lstrip().startswith("(")]
    calls += [m for m in re.finditer(r"\bvkGet[A-Za-z0-9_]*", code)
              if m.group(0) not in void_entry_points
              and code[m.end():].lstrip().startswith("(")]
    bad = []
    for m in sorted(set(calls), key=lambda m: m.start()):
        start = -1
        for ch in (";", "{", "}"):
            start = max(start, code.rfind(ch, 0, m.start()))
        end = code.find(";", m.end())
        if end < 0:
            end = len(code)
        stmt = code[start + 1:end]
        # A COMPARISON against a VK_ constant, not a mention of one. The first version
        # asked whether the statement contained `VK_` at all, and the mutation written
        # to prove this check accepted a bare
        # `vkWaitForFences(..., VK_TRUE, UINT64_MAX);` as read, because VK_TRUE is an
        # ARGUMENT. B5 has the same weakness and is left alone; here the rule is the
        # comparison, which is the thing that actually reads a result.
        if re.search(r"[!=]=[ \t]*VK_|case[ \t]+VK_", stmt):
            continue
        prefix = code[start + 1:m.start()]
        named = re.search(r"(?:VkResult\s+)?(\w+)\s*=\s*$", prefix)
        if named:
            # The value is read if the NEXT thing that touches the name compares it
            # against a VkResult constant. "Next thing that touches it" and not "the
            # next statement": the swapchain-image acquire retries inside an `if` and
            # both attempts share the one test after the block, and a rule that
            # demanded adjacency would have to duplicate that test in the source to
            # satisfy the gate, which is the gate writing the code. It is still not a
            # name match over the whole function: a reassignment before the first
            # comparison means the value was thrown away, and that is red.
            name = named.group(1)
            tail = code[end + 1:end + 400]
            tested = re.search(r"\b" + re.escape(name) + r"\b[ \t]*[!=]=[ \t]*VK_"
                               r"|\bswitch[ \t]*\([ \t]*" + re.escape(name) + r"\b[ \t]*\)",
                               tail)
            moved = re.search(r"\b" + re.escape(name) + r"\b[ \t]*=[ \t]*[^=]", tail)
            if tested and (moved is None or tested.start() < moved.start()):
                continue
        bad.append((line_of(code, m.start()), m.group(0)))
    return calls, bad


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

    A dropped comment line leaves its NEWLINE behind. `line_of` counts newlines in
    the text it is handed, and while comment lines were deleted every line number
    this gate prints — B4's silent returns, B5's unchecked calls, B8's mismatches —
    was a line of the STRIPPED file while the record quoted them as lines of
    `renderer_vk.cpp`: 2355 source lines became 1905, so the most load-bearing
    number in the record could not be opened without re-deriving the offset
    (review-8 F5, and docs/evidence/2026-11-12-152-unfenced-one-shot.txt §5 has the
    eight sites at both numbers).

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
        blank = "\n" if line.endswith("\n") else ""
        if in_block:
            if "*/" in s:
                in_block = False
            out.append(blank)
            continue
        if s.startswith("//"):
            out.append(blank)
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block = True
            out.append(blank)
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


def args_at(text, pos):
    """The split argument list of the call whose name ends at `pos`."""
    j = text.find("(", pos - 1)
    if j < 0:
        return []
    end = close_brace(text, j, "(", ")")
    return split_args(text[j + 1:end]) if end > 0 else []


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


def block_condition(text, brace):
    """The condition text of the `if (...)` whose body opens at `brace`, or '' when
    the block is not an if-body (a function body, a bare block, an `else`)."""
    if brace < 0:
        return ""
    pre = text[:brace].rstrip()
    if not pre.endswith(")"):
        return ""
    open_ = open_brace_back(pre, len(pre) - 1, "(", ")")
    if open_ < 0 or not re.search(r"\bif$", pre[:open_].rstrip()):
        return ""
    return pre[open_ + 1:len(pre) - 1]


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
            figures = [a for a in split_args(call) if INTEGRAL_CAST.search(a)]
            if not figures:
                continue
            checked += 1
            per_site[name] = per_site.get(name, 0) + 1
            src_ops = set(re.findall(r"[/*%]", args[1]))
            src_nums = set(re.findall(r"\d+", args[1]))
            request = " ".join(args[1].split())
            kinds = []
            for f in figures:
                expr = INTEGRAL_CAST.sub(" ", f)
                toks = tokens(expr)
                # The frames-in-flight count is the one extra factor that belongs in
                # a request: the create call is per slot and the line prints the
                # per-slot figure, the total, and the count that turns one into the
                # other. Each of the three is a different shape and each is checked
                # against the request; what may not appear is a fourth shape.
                flat = re.sub(r"\*?\s*(?:mdk::vk::)?kFramesInFlight\s*\*?", " ", expr)
                ftoks = tokens(flat)
                ops = set(re.findall(r"[/*%]", flat)) - src_ops
                nums = set(re.findall(r"\d+", flat)) - src_nums
                if toks and toks <= EXTRA_SIZE_TOKENS:
                    kinds.append("frames")
                    if nums or ops or "kFramesInFlight" not in toks:
                        bad.append("%s prints `%s` where the frames-in-flight count "
                                   "belongs: the line says `x N frames in flight` and "
                                   "that N is not the count the request was made of"
                                   % (name, " ".join(f.split())))
                elif not (want <= toks and toks <= want | EXTRA_SIZE_TOKENS):
                    bad.append("%s prints a figure built from `%s`, not from the `%s` it "
                               "handed the create call"
                               % (name, " ".join(sorted(toks)) or "nothing at all",
                                  request))
                elif ftoks != want or ops or nums:
                    bad.append("%s rescales its figure: it prints `%s` for a request of "
                               "`%s`, so the bytes in the log disagree with what the "
                               "driver was asked for by that factor"
                               % (name, " ".join(flat.split()), request))
                else:
                    kinds.append("size" if ftoks == toks else "total")
            if "size" not in kinds:
                bad.append("%s never prints the `%s` it handed the create call, so the "
                           "line describes some other allocation" % (name, request))
            if "frames" in kinds and "total" not in kinds:
                bad.append("%s prints a frames-in-flight count with no total: the line "
                           "says `x N frames in flight` while the largest byte figure in "
                           "it is one slot, which is review-5 F3's disagreement in the "
                           "other direction" % name)
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
    nulls = [m.start() for m in re.finditer(
        r"\breturn[ \t]+\(?[ \t]*VK_NULL_HANDLE[ \t]*\)?[ \t]*;", code)]
    # A null handle handed back through a named local is the same return with a step
    # through a variable. The discovery used to match one spelling, which made the
    # rule satisfiable by retyping the return: delete the refusal, write
    # `return (VK_NULL_HANDLE);` or `VkDescriptorSet none = VK_NULL_HANDLE; return
    # none;`, and the gate that exists to make a null handle loud said nothing
    # (review-7 F4).
    for lname in sorted({m.group(1) for m in re.finditer(
            r"\bVk\w+[ \t]+(\w+)[ \t]*=[ \t]*VK_NULL_HANDLE[ \t]*;", code)}):
        nulls += [m.start() for m in re.finditer(
            r"\breturn[ \t]+" + lname + r"[ \t]*;", code)]
    for pos in sorted(set(nulls)):
        if not refusal_for_return(code, pos):
            silent.append("line %d" % line_of(code, pos))
    say(CHK["B4"], bool(nulls) and not silent,
        "a null handle is a resource the caller does not have; a file that returns one "
        "without saying so is the silent missing picture this gate exists to stop" +
        ("" if not silent else " — silent at " + ", ".join(silent)))

    # B5: an unchecked VkResult is an undefined out-parameter or an unexecuted
    # submission. The obligation used to be keyed on `vkAllocate*` alone, which is how
    # `vkEndCommandBuffer`, `vkQueueSubmit`, `vkQueueWaitIdle` and
    # `vkCreatePipelineCache` sat outside it with their results thrown away — the gate
    # named the verb it had been written against instead of the property (review-7 F1).
    # So the set is the naming convention: an entry point in a result-bearing family
    # returns a VkResult. The void families are named where RESULT_VERBS is defined.
    unchecked = []
    calls = [m for m in re.finditer(r"\bvk(?:%s)[A-Za-z0-9_]*" % "|".join(RESULT_VERBS),
                                    code)
             if code[m.end():].lstrip().startswith("(")]
    # `vkGet*` is NOT a void family, so the void ones are named one at a time and
    # every other `vkGet` in the file is result-bearing until that list says
    # otherwise (review-8 F3).
    calls += [m for m in re.finditer(r"\bvkGet[A-Za-z0-9_]*", code)
              if m.group(0) not in VOID_ENTRY_POINTS
              and code[m.end():].lstrip().startswith("(")]
    for m in sorted(calls, key=lambda m: m.start()):
        start = -1
        for ch in (";", "{", "}"):
            start = max(start, code.rfind(ch, 0, m.start()))
        end = code.find(";", m.end())
        stmt = code[start + 1:end if end > 0 else len(code)]
        if "VK_SUCCESS" not in stmt:
            unchecked.append("line %d (%s)" % (line_of(code, m.start()), m.group(0)))
    say(CHK["B5"], bool(calls) and not unchecked,
        "a discarded VkResult leaves a handle undefined or a submission unexecuted, and "
        "the next call uses it either way" +
        ("" if not unchecked else " — unchecked at " + ", ".join(unchecked)))

    # B6: the one refusal that used to report SUCCESS. `createHostImage` made the
    # image, failed to get a command buffer to fill and transition it, and returned
    # true: a texture of undefined bytes with no failure anywhere (review-5 F1).
    #
    # This check used to find the `else` sibling of `if (cb)` and look for a refusal in
    # it, which pinned one shape instead of the property; then it pinned "any block
    # between the read and the `return true` that refuses and returns false", which
    # pinned a block and not its CONDITION. Reverting `if (!uploaded)` to `if (!cb)` —
    # literally the shape this check was rewritten away from, and one token — left B6
    # green while review-7 F1 walked back in, because a block that refuses for the
    # wrong reason is not a guard (review-8 F2). So what is pinned is three things:
    # the one-shot result is CAPTURED in a variable, some block's CONDITION tests that
    # variable for "not done" — the one test that is true for every bad outcome, which
    # is why a test naming a single bad outcome does not count — and that block refuses
    # and returns false before the single `return true`.
    filled = False
    sp = body_span(code, "createHostImage")
    if sp:
        body = code[sp[0]:sp[1]]
        read = re.search(r"(\w+)[ \t]*=[ \t]*endOneShot\(", body)
        inline = re.search(r"if[ \t]*\([ \t]*![ \t]*endOneShot\(", body)
        rets = [sp[0] + m.start() for m in re.finditer(r"\breturn[ \t]+true[ \t]*;", body)]
        test = None
        if read:
            var = re.escape(read.group(1))
            test = re.search(r"\b" + var + r"[ \t]*!=[ \t]*OneShot::done\b|![ \t]*" + var + r"\b",
                             body)
        guard = None
        if len(rets) == 1:
            if test:
                needle = " ".join(test.group(0).split())
                for cond, block, spn in blocks(code, sp[0], sp[1]):
                    if (needle in " ".join(cond.split()) and "refused(" in block
                            and "return false" in block and spn[1] <= rets[0]):
                        guard = spn
            elif inline:
                guard = inline  # `if (!endOneShot(...))` is its own guard
        filled = bool(guard)
    say(CHK["B6"], filled,
        "an image whose fill and layout transition were never executed is not a texture, "
        "and saying it is one is a refusal the log never sees (review-5 F1, review-7 F1, "
        "review-8 F2: the guard's condition is the part that was unpinned)")

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
    # AND AT THE CALL SITE. The defect was a PAIRING, and a pairing lives where the two
    # halves meet. Both callee bodies stay exactly as correct as they are while a caller
    # hands `endOneShot` a pool that is not the one `beginOneShotTracked` filled: with
    # `endOneShot(ctx, VK_NULL_HANDLE, cb)` — one token, the kind a bad merge or a
    # tidy-up produces — every upload frees a foreign command buffer into handle zero
    # and destroys handle zero, and a check that read only the two callee bodies stayed
    # green (review-7 F2).
    handovers = []
    begins = []
    for m in re.finditer(r"\bbeginOneShotTracked\s*\(", code):
        if sp and sp[0] <= m.start() < sp[1]:
            continue  # the definition, not a call
        a = args_at(code, m.end())
        mm = re.search(r"&\s*(\w+)", a[1]) if len(a) >= 2 else None
        handovers.append((m.start(), mm.group(1) if mm else None))
        lhs = re.search(r"(\w+)[ \t]*=[ \t]*$", code[:m.start()])
        begins.append((m.start(), lhs.group(1) if lhs else None))
    mismatched = []
    ended = 0
    for m in re.finditer(r"\bendOneShot\s*\(", code):
        if esp and esp[0] <= m.start() < esp[1]:
            continue
        ended += 1
        a = args_at(code, m.end())
        pool = a[1].strip() if len(a) >= 2 else ""
        prior = [ident for pos, ident in handovers if pos < m.start() and ident]
        if not prior or pool != prior[-1]:
            mismatched.append("line %d (`%s`)" % (line_of(code, m.start()),
                                                  pool or "no pool argument"))
    if ended != len(handovers):
        mismatched.append("%d begin calls but %d end calls: a one-shot pool that is "
                          "never ended is never destroyed" % (len(handovers), ended))
    # AND NOTHING RETURNS BETWEEN THE TWO. Counting call sites proves the pairing
    # exists, not that it is reached: a `return` inserted between the begin and the end
    # strands the pool and the command buffer exactly as the file-scope ledger did, and
    # two equal counts never notice (review-8 F4). The one early exit that IS correct
    # is the one the callee already cleaned up after — `if (!cb)`, when
    # `beginOneShotTracked` destroyed the pool on its way out — so every return in that
    # range has to be guarded on the command buffer it did not get.
    stranded_exit = []
    for m in re.finditer(r"\bendOneShot\s*\(", code):
        if esp and esp[0] <= m.start() < esp[1]:
            continue
        prior = [b for b in begins if b[0] < m.start()]
        if not prior or not prior[-1][1]:
            continue
        bpos, cbident = prior[-1]
        for rm in re.finditer(r"\breturn\b", code[bpos:m.start()]):
            pos = bpos + rm.start()
            cond = block_condition(code, enclosing_brace(code, pos))
            if not re.search(r"![ \t]*" + cbident + r"\b|" + cbident +
                             r"[ \t]*==[ \t]*VK_NULL_HANDLE", cond):
                stranded_exit.append("line %d (unguarded %s exit)" % (
                    line_of(code, pos), cbident))
    say(CHK["B8"], bool(sp) and bool(esp)
        and "*outPool = pool" in obody
        and "vkFreeCommandBuffers(ctx->device, pool" in ebody
        and "vkDestroyCommandPool(ctx->device, pool" in ebody
        and "[0]" not in ebody
        and not stranded and not mismatched and not stranded_exit,
        "a command buffer freed into a pool it did not come from, and that pool "
        "destroyed under it, is corruption rather than a missing picture; pairing "
        "them by position let one refusal desynchronize the pairing for the rest of "
        "the run" + ("" if not stranded else " — pool stranded at " + ", ".join(stranded))
        + ("" if not mismatched else " — pool mismatched at " + ", ".join(mismatched))
        + ("" if not stranded_exit else " — a return between the begin and the end "
                                         "strands the pair at " + ", ".join(stranded_exit)))

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

    # B10: the third one-shot outcome owns something. `endOneShot` can now say
    # "accepted, completion unknown", and the only correct answer is to stop treating
    # what the submission names as this side of the boundary. Review-8 F1 was the
    # refusal that printed "the staging memory it read is not yet safe to release"
    # and freed it on the next line, in both callers: a failed wait is a completion
    # boundary that was NOT established, and AGENTS.md P3 says do not free an in-flight
    # resource. So the block that handles the unfenced outcome must contain no release
    # call at all — not a free, not a destroy, not an unmap. Abandoning ownership is
    # the fix; the handles get nulled because the function stopped owning them, and
    # that is an assignment, not a release.
    bad10 = []
    for fn in ("createHostImage", "captureSwapchain"):
        fsp = body_span(code, fn)
        handled = False
        if fsp:
            for cond, block, _spn in blocks(code, fsp[0], fsp[1]):
                if "OneShot::unfenced" not in cond or "return false" not in block:
                    continue
                released = [c for c in ("vkFreeMemory(", "vkDestroyBuffer(",
                                        "vkDestroyImage(", "vkUnmapMemory(") if c in block]
                if released:
                    bad10.append("%s releases %s inside the block that says the "
                                 "submission may still be running" % (fn, released[0]))
                else:
                    handled = True
        if not handled and not bad10:
            bad10.append("%s has no block that refuses an unfenced submission" % fn)
    # ... and the CALLEE. `endOneShot` owns the command buffer and the pool the
    # submission is made of, and it used to free both on the way out of the unfenced
    # case: `vkFreeCommandBuffers` and `vkDestroyCommandPool` each require that the
    # submitted commands have completed, which is the one fact the unfenced outcome
    # says is not known (review-1 F3). The guard has to sit BEFORE the releases, and
    # "before" is only "unreachable" in straight-line code, so the absence of a loop is
    # part of what is pinned rather than assumed.
    fsp = body_span(code, "endOneShot")
    if not fsp:
        bad10.append("endOneShot is not in the file")
    else:
        body = code[fsp[0]:fsp[1]]
        guard = None
        for cond, _block, spn in blocks(code, fsp[0], fsp[1]):
            if "OneShot::unfenced" in cond and re.search(r"\breturn\b", _block):
                guard = spn[0] - fsp[0]
                break
        releases = [m.start() for m in re.finditer(
            r"vkFreeCommandBuffers\s*\(|vkDestroyCommandPool\s*\(", body)]
        if guard is None:
            bad10.append("endOneShot has no early return keeping an unfenced "
                         "submission's command buffer and pool out of the releases")
        elif re.search(r"\bfor\b|\bwhile\b|\bgoto\b", strip_code(body)):
            bad10.append("endOneShot is no longer straight-line, so the guard's "
                         "ordering no longer proves the releases are unreachable")
        elif releases and min(releases) < guard:
            bad10.append("endOneShot frees the command buffer or destroys its pool "
                         "before the unfenced guard, at line %d"
                         % line_of(code, fsp[0] + min(releases)))
    say(CHK["B10"], not bad10,
        "a submission the wait could not prove finished may still be reading or writing "
        "what it names; freeing it there is the use-after-free the refusal's own words "
        "warned about (review-8 F1, review-1 F3, AGENTS.md P3)" + ("" if not bad10 else " — " + "; ".join(bad10)))

    # B11: the frame seam reads BOTH of its results, and says so.
    #
    # `beginFrame` and `endFrame` each return bool. The seam used to `return` from the
    # first without a word and throw the second away outright, so a frame that never
    # began or never submitted was indistinguishable from one that drew: same return
    # value, same counters, and — worst — the same screenshot, read back from a
    # swapchain image this frame never wrote and then published as the pinned record.
    # The context file refuses quietly on purpose (it has no per-request memory and a
    # frame-boundary failure repeats every frame); the seam is the layer that owns
    # refusal reporting, so the seam is what this pins.
    bad11 = []
    fsp = body_span(code, "mdk_render_begin_frame")
    begun = False
    if fsp:
        for cond, block, _spn in blocks(code, fsp[0], fsp[1]):
            if "mdk::vk::beginFrame(" not in cond:
                continue
            head = block[:block.find("return")] if "return" in block else block
            if "refused(" in head:
                begun = True
    if not begun:
        bad11.append("mdk_render_begin_frame drops the frame without refusing before "
                     "the return")
    fsp = body_span(code, "mdk_render_end_frame")
    named = re.search(r"(\w+)[ \t]*=[ \t]*mdk::vk::endFrame[ \t]*\(", code[fsp[0]:fsp[1]]) if fsp else None
    if not named:
        bad11.append("mdk_render_end_frame does not capture endFrame's result — a "
                     "discarded bool is a frame that cannot fail")
    else:
        name = named.group(1)
        told = False
        for cond, block, _spn in blocks(code, fsp[0], fsp[1]):
            # `blocks()` hands back the condition WITHOUT its closing paren, so the
            # name has to be bounded by "not an identifier character" rather than by
            # a bracket.
            if re.search(r"![ \t]*" + re.escape(name) + r"(?![\w])", cond) and "refused(" in block:
                told = True
        if not told:
            bad11.append("nothing tells the operator that %s was false" % name)
        # The capture has to sit behind the frame's own result: a BMP of an image the
        # frame never wrote reads exactly like a BMP of the frame, and a frame record
        # that cannot be trusted is worse than none, because one gets committed.
        shot = code.find("captureSwapchain(", fsp[0])
        empty = code.find("screenshotPath.empty()", fsp[0])
        if shot > empty and re.search(r"\b" + name + r"\b", code[empty:shot]) is None:
            bad11.append("the screenshot is captured without consulting %s, so a frame "
                         "that never submitted can still be published" % name)
    say(CHK["B11"], not bad11,
        "a frame whose begin, submit or present failed is not a frame; the seam is the "
        "only layer that can say so, and its screenshot is the record the loop commits" +
        ("" if not bad11 else " — " + "; ".join(bad11)))

    # B12: a descriptor set the cache forgets goes back to the pool, and not before the
    # frame that named it has finished.
    #
    # `forgetDescriptorSets` is review-25's fix for a cached set naming a destroyed
    # image view, and it is correct: the entry has to leave the cache. But a CACHE ENTRY
    # and an ALLOCATED SET are two things, and until this batch only the first one was
    # released. `vkFreeDescriptorSets` appeared nowhere in the backend and
    # `vkResetDescriptorPool` appeared nowhere, so a pool built on a fixed `maxSets`
    # spent one set on every texture or palette released and re-made — and
    # `mdk_render_update_texture` does exactly that on every same-size update — for the
    # life of the process. When the pool emptied, the B4 refusal started skipping every
    # draw that needed a new pair, once per distinct request and then silently.
    #
    # The three shapes below are pinned, because there are three distinct ways to be
    # wrong about the parts. Review-9 F1 was a fourth: the parts can each be present
    # and the JOIN between them still broken, and four mutants proved it — a retire
    # that records nothing, a settle that never erases, a deadline one frame early,
    # and a drop site that retires somebody else's set. Each of those printed PASS
    # here, and the first is the exact leak this batch exists to close. So the join is
    # pinned too, one shape per mutant, with an arm apiece:
    #
    #   1. the pool must be created with the bit that makes its sets freeable.
    #      `vkFreeDescriptorSets` is invalid use on a pool that did not ask for it, so
    #      the obvious one-line fix is a second bug unless the create says so; and the
    #      alternative, `vkResetDescriptorPool`, takes every set in the pool with it,
    #      including the ones a queued frame has bound, so it has no safe point in a
    #      frame loop;
    #   2. the drop site must RETIRE the set and must not free it there. A set that a
    #      recorded frame bound is in use, and AGENTS.md P3 puts the completion boundary
    #      before the CPU release — the same reason `retireBuffer` exists;
    #   3. the settle must actually free it, on the deadline the buffers already wait
    #      for, and read the result. `vkFree*` is a MIXED family — `vkFreeMemory` and
    #      `vkFreeCommandBuffers` are void, `vkFreeDescriptorSets` is not — and B5's verb
    #      list does not cover `Free`, so nothing else in this file would notice the
    #      result thrown away. Closing that gap properly is B5's job and would move B5,
    #      which this file's own precedent forbids while a new check is being introduced;
    #      the obligation is pinned here instead and the gap is recorded in CONTINUATION.
    bad12 = []
    pool = re.search(r"VkDescriptorPoolCreateInfo\s+(\w+)\s*=\s*\{\}", code)
    if not pool:
        bad12.append("no VkDescriptorPoolCreateInfo is built in the file, so the pool "
                     "this rule is about is not the pool being checked")
    else:
        pname = pool.group(1)
        made = code.find("vkCreateDescriptorPool(", pool.end())
        head = code[pool.end():made] if made > 0 else code[pool.end():pool.end() + 2000]
        if not re.search(re.escape(pname) + r"\.flags[ \t]*=[^;]*FREE_DESCRIPTOR_SET",
                         head):
            bad12.append("%s never sets .flags to VK_DESCRIPTOR_POOL_CREATE_FREE_"
                         "DESCRIPTOR_SET_BIT before the create, so returning a set to "
                         "this pool is invalid use" % pname)

    fsp = body_span(code, "forgetDescriptorSets")
    if not fsp:
        bad12.append("forgetDescriptorSets is not in the file")
    else:
        body = code[fsp[0]:fsp[1]]
        if "retireDescriptorSet(" not in body:
            bad12.append("forgetDescriptorSets drops the cache entry and never retires "
                         "the set it dropped — the pool loses one set per drop")
        else:
            # THE ARGUMENT IS THE ENTRY BEING DROPPED. `retireDescriptorSet(r, X)`
            # reads the same wherever X comes from, and a call that hands over the
            # last cached set, or none, keeps every shape below it green while the
            # dropped set is still lost (review-9 F1 mutant 4). The entry it drops is
            # the one the swap-pop overwrites, so that is the identity required.
            hit = re.search(r"retireDescriptorSet\([ \t]*\w+[ \t]*,[ \t]*"
                            r"(\w+(?:->\w+)+)\[(\w+)\]\.set[ \t]*\)", body)
            if not hit:
                bad12.append("forgetDescriptorSets does not retire the `.set` of the cache "
                             "entry it is dropping, so some other set — or none — is what "
                             "goes back to the pool")
            elif not re.search(re.escape(hit.group(1)) + r"\[" + re.escape(hit.group(2)) +
                               r"\][ \t]*=[ \t]*" + re.escape(hit.group(1)) + r"\.back\(\)",
                               body[hit.end():]):
                bad12.append("`%s[%s].set` is retired but that slot is not the one the "
                             "swap-pop overwrites, so the handle handed to the pool and the "
                             "entry dropped from the cache are two different sets"
                             % (hit.group(1), hit.group(2)))
        for call in ("vkFreeDescriptorSets", "vkResetDescriptorPool"):
            if call + "(" in body:
                bad12.append("forgetDescriptorSets calls %s at the drop site: a set a "
                             "frame has already recorded is still in use there" % call)

    rsp = body_span(code, "retireDescriptorSet")
    if not rsp:
        bad12.append("retireDescriptorSet is not in the file")
    else:
        rbody = code[rsp[0]:rsp[1]]
        # EXACTLY `frameIndex + kFramesInFlight`, with nothing after it. The loose
        # form of this rule matched `... + kFramesInFlight - 1`, which is the
        # off-by-one that frees a set the just-submitted frame binds: retire in
        # frame N, free at begin(N+1), and the fence waited there is frame N-1's
        # (review-9 F1 mutant 3).
        if not re.search(r"safeFrame[ \t]*=[ \t]*\w+->frameIndex[ \t]*\+[ \t]*"
                         r"(?:mdk::vk::)?kFramesInFlight[ \t]*;", rbody):
            bad12.append("retireDescriptorSet's deadline is not exactly frameIndex + "
                         "kFramesInFlight with no offset, so it is not the boundary "
                         "beginFrame's fence actually proves")
        # AND IT MUST RECORD THE SET. A helper that builds a RetiredSet, names its
        # deadline, and drops the local on the floor leaves every other shape here
        # green while the leak runs at full size (review-9 F1 mutant 1).
        if "retiringSets.push_back(" not in rbody:
            bad12.append("retireDescriptorSet never records the set it was handed — the "
                         "call at the drop site is a function call and nothing more, and "
                         "the pool loses one set per drop")

    ssp = body_span(code, "settleRetired")
    if not ssp:
        bad12.append("settleRetired is not in the file")
    else:
        body = code[ssp[0]:ssp[1]]
        idx = body.find("retiringSets")
        if idx < 0:
            bad12.append("settleRetired never settles retiringSets — the retired sets "
                         "are collected and then dropped with the vector")
        else:
            tail = body[idx:]
            if "safeFrame" not in tail:
                bad12.append("settleRetired frees retired sets without comparing their "
                             "safeFrame, so it frees sets a queued frame may still bind")
            # FREE IT, THEN REMOVE IT. A settle that frees and leaves the entry
            # frees the same handle on every frame after it — and once the pool has
            # handed that handle back out it is freeing a live set (review-9 F1
            # mutant 2). Removing it before the free would lose a handle whose free
            # failed, so the order is part of the rule.
            free_at = tail.find("vkFreeDescriptorSets(")
            erase_at = tail.find("retiringSets.erase(")
            if erase_at < 0:
                bad12.append("settleRetired frees a retired set and never removes it from "
                             "the list, so every later frame frees that handle again and, "
                             "once the pool reissues it, frees a live set")
            elif free_at >= 0 and erase_at < free_at:
                bad12.append("settleRetired removes the entry before freeing the set, so a "
                             "failed free loses a handle the pool never got back")
            m = re.search(r"(\w+)[ \t]*=[ \t]*vkFreeDescriptorSets[ \t]*\(", tail)
            inline = re.search(r"if[ \t]*\([ \t]*vkFreeDescriptorSets[ \t]*\(", tail)
            if "vkFreeDescriptorSets(" not in tail:
                bad12.append("settleRetired never calls vkFreeDescriptorSets — that is "
                             "the leak again with a vector in front of it")
            elif not inline and not m:
                bad12.append("settleRetired calls vkFreeDescriptorSets and throws its "
                             "VkResult away; vkFree* is a mixed family and B5's verbs do "
                             "not cover Free, so nothing else here would notice")
            elif m and not inline:
                nm = m.group(1)
                rest = tail[m.end():m.end() + 400]
                if not re.search(r"\b" + re.escape(nm) + r"\b[^;]*VK_", rest) \
                        and not re.search(r"if[ \t]*\([ \t]*" + re.escape(nm), rest):
                    bad12.append("settleRetired discards vkFreeDescriptorSets' VkResult "
                                 "in `%s`; vkFree* is a mixed family and B5's verbs do "
                                 "not cover Free, so nothing else here would notice" % nm)
    say(CHK["B12"], not bad12,
        "a descriptor set is a slot in a pool with a fixed budget; forgetting the cache "
        "entry frees nothing, freeing it at the drop site is use-after-free, and freeing "
        "it on a pool that never asked for freeable sets is invalid use; and a retire "
        "that records nothing, a settle that never erases, and a deadline one frame "
        "early are all three of those failures wearing the fix's clothes" +
        ("" if not bad12 else " — " + "; ".join(bad12)))


def context_checks(r, text, quiet):
    """The Linux Vulkan context: the same result rule as B5, over the file review-8 F3
    named as the place it still lived. See `unchecked_result_calls` for why the rule
    is B5's plus this file's assignment-then-test idiom."""
    code = strip_code(text)

    def say(label, ok, detail=""):
        if quiet:
            r.quiet(label, ok)
        else:
            r.check(label, ok, detail)

    calls, bad = unchecked_result_calls(code, CONTEXT_VOID_ENTRY_POINTS)
    say(CHK["C1"], bool(calls) and not bad,
        "a discarded VkResult leaves an out-parameter holding whatever the initialiser "
        "put there and a submission unexecuted, and the next call cannot tell the two "
        "apart" +
        ("" if not bad else " — unchecked at " +
         ", ".join("line %d (%s)" % t for t in bad)))

    # C2: the frame's command buffer is ENDED before it is SUBMITTED. `endFrame` begins
    # nothing — `beginFrame` did that — and a buffer that was begun and never ended is
    # still in the RECORDING state, which `VkSubmitInfo::pCommandBuffers` may not name
    # (VUID-vkSubmitInfo-pCommandBuffers-00075). It was submitted like that every frame
    # and drew, because lavapipe does not enforce the state machine: 138 committed
    # screenshots are evidence about Mesa and none about the submission (review-1 F1).
    # That its result is read is C1's business; what C2 pins is that the call exists and
    # sits on the near side of the submit.
    bad2 = []
    fsp = body_span(code, "endFrame")
    if not fsp:
        bad2.append("endFrame is not in the file")
    else:
        body = code[fsp[0]:fsp[1]]
        ended_at = body.find("vkEndCommandBuffer(")
        submit_at = body.find("vkQueueSubmit(")
        if submit_at < 0:
            bad2.append("endFrame no longer submits anything, so the frame's submit "
                        "moved somewhere this check cannot see")
        elif ended_at < 0:
            bad2.append("the frame's command buffer is submitted without ever being "
                        "ended — an unterminated stream is still RECORDING")
        elif ended_at > submit_at:
            bad2.append("the frame's command buffer is ended at line %d, after the "
                        "submit at line %d"
                        % (line_of(code, fsp[0] + ended_at),
                           line_of(code, fsp[0] + submit_at)))
    say(CHK["C2"], not bad2,
        "a command stream has to be closed before the queue is given it; a driver that "
        "enforces the state machine drops the frame and this file reports it as drawn" +
        ("" if not bad2 else " — " + "; ".join(bad2)))

    # C3: a refused frame never leaves its slot's fence UNSIGNALLED.
    #
    # Review-1 F2 read the four `return false` exits after `beginFrame`'s fence wait as
    # hanging the next frame, on the premise that the wait resets the fence. It does
    # not: only `vkResetFences` does, and that is the one call that would make the
    # reviewer's scenario real. So the premise is pinned instead of argued — no reset
    # in `beginFrame`, and the one reset that exists, in `endFrame` immediately before
    # the submit, has to put a SIGNALED fence back when the submit is what failed, which
    # is the hazard batch 153 did find and did fix.
    bad3 = []
    bsp = body_span(code, "beginFrame")
    if not bsp:
        bad3.append("beginFrame is not in the file")
    elif "vkResetFences(" in code[bsp[0]:bsp[1]]:
        bad3.append("beginFrame resets the slot's fence, which is the only way a refused "
                    "begin can leave an unsignalled fence for the next frame to wait on "
                    "with UINT64_MAX forever")
    fsp = body_span(code, "endFrame")
    if fsp:
        body = code[fsp[0]:fsp[1]]
        submit_at = body.find("vkQueueSubmit(")
        tail = body[submit_at:] if submit_at >= 0 else ""
        if submit_at < 0:
            bad3.append("endFrame no longer submits, so where its fence goes is unknown")
        elif "VK_FENCE_CREATE_SIGNALED_BIT" not in tail or \
                re.search(r"frame\.inFlight\s*=\s*(?!VK_NULL_HANDLE)\w+", tail) is None:
            bad3.append("endFrame's refused submit leaves the slot's fence reset and "
                        "unsignalled: the next beginFrame waits UINT64_MAX on a fence no "
                        "submission will ever signal")
    say(CHK["C3"], not bad3,
        "the invariant is 'this slot's fence is signalled whenever the slot is free'; a "
        "frame that breaks it is one frame late and a hang two frames later" +
        ("" if not bad3 else " — " + "; ".join(bad3)))

    # C4: a frame that took an image and then refused CONSUMES the acquire's signal.
    # `presentWait` is a binary semaphore — created with no type info, so it holds a
    # state and not a count — and the only thing that clears a posted one is a submit
    # that waits on it. A refusal that leaves it posted makes the NEXT frame's acquire
    # invalid use (VUID-vkAcquireNextImageKHR-semaphore-01779). The record used to call
    # the leftover "a stale +1 ... not a leak and not a hang", which is arithmetic the
    # object cannot do (review-1 F4).
    def drains(pos):
        """Does the if-body this `return false` sits in consume presentWait?

        The block has to be an if-body: the enclosing brace of a bare statement at
        function scope is the function itself, and a drain anywhere in the function
        would then vouch for a return that never reaches it.
        """
        brace = enclosing_brace(code, pos)
        if brace < 0 or not block_condition(code, brace):
            return False
        sp = block_span_from(code, brace)
        return sp is not None and "drainPresentWait(" in code[brace:sp[1]]

    bad4 = []
    if bsp:
        body = code[bsp[0]:bsp[1]]
        taken = body.find("ctx->currentImage = imageIndex;")
        if taken < 0:
            bad4.append("beginFrame no longer records the image it acquired, so a "
                        "refused acquire cannot be told from a refused frame")
        else:
            for m in re.finditer(r"\breturn[ \t]+false[ \t]*;", body[taken:]):
                pos = bsp[0] + taken + m.start()
                if not drains(pos):
                    bad4.append("line %d refuses a frame that already holds an image "
                                "without consuming the acquire's signal"
                                % line_of(code, pos))
    if fsp:
        body = code[fsp[0]:fsp[1]]
        present_at = body.find("vkQueuePresentKHR(")
        # Before the present is where the submit lives: once the queue has the frame,
        # presentWait was waited on and there is nothing left to consume.
        upto = present_at if present_at >= 0 else len(body)
        for m in re.finditer(r"\breturn[ \t]+false[ \t]*;", body[:upto]):
            pos = fsp[0] + m.start()
            if not drains(pos):
                bad4.append("line %d refuses a frame whose image was acquired without "
                            "consuming the acquire's signal" % line_of(code, pos))
    say(CHK["C4"], not bad4,
        "a binary semaphore left posted is not a spare credit, it is the next frame's "
        "invalid acquire" + ("" if not bad4 else " — " + "; ".join(bad4)))


def adjudicate(who, text, quiet=False):
    r = Result(who)
    if who.startswith("Linux frontend"):
        frontend_checks(r, text, quiet)
    elif who.startswith("Linux context"):
        context_checks(r, text, quiet)
    else:
        backend_checks(r, text, quiet)
    return r


FILES = {}
for who, rel in (("Linux frontend", FRONTEND), ("Linux backend", BACKEND),
                 ("Linux context", CONTEXT)):
    try:
        FILES[who] = open(ROOT + "/" + rel, errors="replace").read()
    except OSError as exc:
        print("  FAIL linux_refusal_report: cannot read %s (%s)" % (rel, exc))
        sys.exit(1)

real = [adjudicate("Linux frontend", FILES["Linux frontend"]),
        adjudicate("Linux backend", FILES["Linux backend"]),
        adjudicate("Linux context", FILES["Linux context"])]

# ---------------------------------------------------------------------------
# The arms. Each is a realistic edit that COMPILES — a mutant that would not build
# is a mutant of nothing (review-15 F3) — and each names the one check that must
# catch it.
# ---------------------------------------------------------------------------
FRONT_ARMS = []
BACK_ARMS = []
CTX_ARMS = []


def arm_context(label, mutate, expected):
    CTX_ARMS.append((label, mutate, CHK[expected]))


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
        "OneShot endOneShot(mdk::vk::Context* ctx, VkCommandPool pool, VkCommandBuffer cb) {",
        "std::vector<VkCommandPool> g_oneShotPools;\n\n"
        "OneShot endOneShot(mdk::vk::Context* ctx, VkCommandPool pool, VkCommandBuffer cb) {", 1)
    if out == text:
        return text
    return out.replace("    vkFreeCommandBuffers(ctx->device, pool, 1, &cb);",
                       "    vkFreeCommandBuffers(ctx->device, g_oneShotPools[0], 1, &cb);", 1)


def be_oneshot_call_site_passes_null(text):
    """Both call sites hand `endOneShot` handle zero instead of the pool they were
    given, which is one token and the kind of thing a bad merge or a tidy-up does. The
    two callee bodies are untouched, so a check that reads only them — as B8 did until
    review-7 F2 — stays green while every upload frees a foreign command buffer into
    handle zero and destroys handle zero. B8 only."""
    out = text.replace("endOneShot(ctx, oneShotPool, cb)",
                       "endOneShot(ctx, VK_NULL_HANDLE, cb)", 1)
    out = out.replace("endOneShot(&r->ctx, oneShotPool, cb)",
                      "endOneShot(&r->ctx, VK_NULL_HANDLE, cb)", 1)
    return out if out != text else text


def be_upload_result_discarded(text):
    """`endOneShot` can fail now and the caller stops listening: the upload is assumed
    to have run, which is review-5 F1 back again through the door review-7 F1 opened.
    B6 only."""
    return text.replace("upload = endOneShot(ctx, oneShotPool, cb);",
                        "endOneShot(ctx, oneShotPool, cb);\n        upload = OneShot::done;", 1)


def be_unfilled_guard_condition_reverted(text):
    """The one-token revert review-8 F2 named. The guard that decides whether the image
    was filled goes back to testing the COMMAND BUFFER, which is non-null even when the
    submit was refused: the block still refuses and still returns false, so a check
    that reads the block and not its condition — as B6 did until review-8 F2 — stays
    green while review-7 F1 walks back in. B6 only."""
    return text.replace("    if (upload != OneShot::done) {", "    if (!cb) {", 1)


def be_unfenced_pair_freed(text):
    """The finding itself, restored: the block whose own refusal says the copy may
    still be running frees the staging memory on its way out — the use-after-free
    behind a log line that said it was not safe (review-8 F1). B10 only."""
    return text.replace('''                width, height, (unsigned long long)size);
        *outImage = VK_NULL_HANDLE;''',
                        '''                width, height, (unsigned long long)size);
        vkFreeMemory(ctx->device, stagingMemory, nullptr);
        *outImage = VK_NULL_HANDLE;''', 1)


def be_oneshot_early_return_strands_pair(text):
    """A guard that returns BETWEEN the begin and the end. The call counts still agree,
    both callee bodies are untouched, and the pool `beginOneShotTracked` just made is
    never ended — review-6 F2's stranding through the caller instead of the ledger
    (review-8 F4). B8 only."""
    return text.replace('''    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;''',
                        '''    if (width != height) {
        return false;
    }
    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;''', 1)


def be_get_result_dropped(text):
    """A result-bearing `vkGet*` discarded. The comment this gate used to carry called
    `vkGet*` a void family, and it is not one: `vkGetFenceStatus` returns a VkResult,
    and a gate that excludes a family by verb cannot see its results thrown away
    (review-8 F3). B5 only."""
    return text.replace('''    vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &memProps);''',
                        '''    vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &memProps);
    vkGetFenceStatus(ctx->device, VK_NULL_HANDLE);''', 1)


def be_submit_result_dropped(text):
    """The one-shot VkResult discarded again — the exact code review-7 F1 found, which
    a gate keyed on `vkAllocate*` could not see. B5 only."""
    return text.replace("const bool ended = vkEndCommandBuffer(cb) == VK_SUCCESS;",
                        "vkEndCommandBuffer(cb);\n    const bool ended = true;", 1)


def be_frames_figure_replaced_by_literal(text):
    """The frames-in-flight count in the line becomes `8`. The request is
    `kFramesInFlight`, which is 2, so the line disagrees with the request by 4x — the
    review-5 F3 defect, in the one number a reader of only `unsigned long long`
    arguments never looked at (review-7 F3). B3 only."""
    return text.replace("(unsigned long long)cap, (unsigned)mdk::vk::kFramesInFlight);",
                        "(unsigned long long)cap, (unsigned)8);", 1)


def be_null_return_respelled(text):
    """The descriptor refusal deleted and its return respelled as
    `return (VK_NULL_HANDLE);`. The callers draw with the PREVIOUS draw's set bound —
    a wrong picture — and a discovery that matches one spelling of a null return does
    not notice (review-7 F4). B4 only."""
    out = text.replace('repeat ? "REPEAT" : "CLAMP");\n        return VK_NULL_HANDLE;',
                       'repeat ? "REPEAT" : "CLAMP");\n        return (VK_NULL_HANDLE);', 1)
    if out == text:
        return text
    return be_delete_descriptor_refusal(out)


def be_null_return_via_local(text):
    """The same refusal deleted with the null handle returned through a named local,
    the other spelling a canonical-text matcher does not see (review-7 F4). B4 only."""
    out = text.replace('repeat ? "REPEAT" : "CLAMP");\n        return VK_NULL_HANDLE;',
                       'repeat ? "REPEAT" : "CLAMP");\n        return set;', 1)
    if out == text:
        return text
    return be_delete_descriptor_refusal(out)


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
arm_backend("oneshot-call-site-passes-null-pool", be_oneshot_call_site_passes_null, "B8")
arm_backend("upload-result-discarded", be_upload_result_discarded, "B6")
arm_backend("unfilled-guard-condition-reverted", be_unfilled_guard_condition_reverted, "B6")
arm_backend("unfenced-pair-freed", be_unfenced_pair_freed, "B10")


def be_unfenced_callee_frees_the_stream(s):
    """Review-1 F3, restored: the callers abandon an unfenced submission's resources and
    the callee goes on freeing the command stream the submission is made of. Only the
    guard in `endOneShot` is removed, so every caller-side arm still passes and only the
    new half of B10 can see it."""
    return s.replace("""    if (result == OneShot::unfenced) {
        refused("one-shot command buffer and its pool are abandoned, not freed: the "
                "submission that may still be running is made of them");
        return result;
    }
""", "", 1)


arm_backend("unfenced-callee-frees-the-stream", be_unfenced_callee_frees_the_stream, "B10")

arm_backend("oneshot-early-return-strands-pair", be_oneshot_early_return_strands_pair, "B8")
arm_backend("result-bearing-vkget-discarded", be_get_result_dropped, "B5")
arm_backend("one-shot-submit-result-dropped", be_submit_result_dropped, "B5")
arm_backend("frames-figure-replaced-by-a-literal", be_frames_figure_replaced_by_literal, "B3")
arm_backend("null-return-respelled", be_null_return_respelled, "B4")
arm_backend("null-return-through-a-named-local", be_null_return_via_local, "B4")
arm_backend("memory-freed-before-destroy", be_free_before_destroy, "B9")


def be_begin_frame_silent(s):
    return s.replace("""    if (!mdk::vk::beginFrame(&r->ctx, w, h, true, clear)) {
        refused("begin the frame at %ux%u: nothing was recorded and nothing will be "
                "presented this frame",
                w, h);
        return;
    }""", """    if (!mdk::vk::beginFrame(&r->ctx, w, h, true, clear)) {
        return;
    }""", 1)


def be_end_frame_result_discarded(s):
    # The exact pre-batch shape: the frame's own result thrown away and the
    # screenshot captured from whatever the swapchain image happened to hold.
    return s.replace("""    const bool submitted = mdk::vk::endFrame(&r->ctx);
    r->inFrame = false;
    if (!submitted) {
        refused("end the frame: its commands were not submitted or not presented, so this "
                "frame did not reach the display");
    }
    if (!r->screenshotPath.empty()) {
        const std::string path = r->screenshotPath;
        r->screenshotPath.clear();
        if (!submitted) {
            // The readback would succeed and print its usual "wrote N" line about an
            // image this frame never drew into. A frame record that cannot be trusted
            // is worse than no frame record, because a frame record gets committed.
            refused("capture the frame to %s: the frame it was asked to capture was never "
                    "submitted, so the image would not be this frame's",
                    path.c_str());
        } else {
            captureSwapchain(r, path.c_str());
        }
    }""", """    mdk::vk::endFrame(&r->ctx);
    r->inFrame = false;
    if (!r->screenshotPath.empty()) {
        const std::string path = r->screenshotPath;
        r->screenshotPath.clear();
        captureSwapchain(r, path.c_str());
    }""", 1)


def be_capture_of_an_unsubmitted_frame(s):
    # The result is read and reported, but the pinned frame is captured anyway: the
    # picture of a frame that never reached the display, published as the record.
    return s.replace("""        if (!submitted) {
            // The readback would succeed and print its usual "wrote N" line about an
            // image this frame never drew into. A frame record that cannot be trusted
            // is worse than no frame record, because a frame record gets committed.
            refused("capture the frame to %s: the frame it was asked to capture was never "
                    "submitted, so the image would not be this frame's",
                    path.c_str());
        } else {
            captureSwapchain(r, path.c_str());
        }""", """        captureSwapchain(r, path.c_str());""", 1)


arm_backend("begin-frame-refusal-deleted", be_begin_frame_silent, "B11")
arm_backend("end-frame-result-discarded", be_end_frame_result_discarded, "B11")
arm_backend("screenshot-of-an-unsubmitted-frame", be_capture_of_an_unsubmitted_frame, "B11")


def ce_fence_wait_result_dropped(s):
    # The exact pre-batch shape, declaration and all: the fence wait was a
    # statement, and `result` belonged to the acquire that followed it.
    return s.replace("""    VkResult result = vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return false;
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(""", """    vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(""", 1)


def ce_begin_command_buffer_result_dropped(s):
    return s.replace("""    result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }""", """    vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);""", 1)


def ce_named_result_never_tested(s):
    # The assignment is there and the TEST is not, which is the shape a name-based
    # scan would accept and the shape that reads nothing.
    return s.replace("""    const VkResult idle = vkDeviceWaitIdle(ctx->device);
    if (idle != VK_SUCCESS) {
        vkFailed("vkDeviceWaitIdle(before the swapchain rebuild; nothing was destroyed)",
                 idle);
        return false;
    }""", """    const VkResult idle = vkDeviceWaitIdle(ctx->device);
    (void)idle;""", 1)


arm_context("fence-wait-result-dropped", ce_fence_wait_result_dropped, "C1")
arm_context("begin-command-buffer-result-dropped", ce_begin_command_buffer_result_dropped, "C1")
arm_context("named-result-never-tested", ce_named_result_never_tested, "C1")


def ce_frame_submitted_unended(s):
    """Review-1 F1, restored: the frame's stream is handed to the queue with no end, and
    every frame still draws on Mesa. The call is gone rather than unchecked, so C1 stays
    green and only C2 can see it."""
    return s.replace("""    const VkResult ended = vkEndCommandBuffer(frame.commandBuffer);
    if (ended != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        vkFailed("vkEndCommandBuffer(the frame's command buffer; nothing was submitted)",
                 ended);
        return false;
    }
""", "", 1)


def ce_fence_reset_in_begin_frame(s):
    """Review-1 F2's premise, made real: a reset in `beginFrame` is what turns a refused
    begin into a fence nobody will ever signal. The result is read, so C1 stays green and
    only C3 can see it."""
    return s.replace("""    VkResult result = vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return false;
    }
""", """    VkResult result = vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return false;
    }
    const VkResult rearmed = vkResetFences(ctx->device, 1, &frame.inFlight);
    if (rearmed != VK_SUCCESS) {
        return false;
    }
""", 1)


def ce_refused_submit_keeps_the_reset_fence(s):
    """The hazard batch 153 did find: the submit failed after the reset, so the slot's
    fence is unsignalled and the next beginFrame waits forever. The recovery is deleted
    and its VkResult goes with it, so C1 has nothing to complain about and only C3 can
    see it."""
    return s.replace("""        VkFence replacement = VK_NULL_HANDLE;
        VkFenceCreateInfo recover = {};
        recover.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        recover.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult refence = vkCreateFence(ctx->device, &recover, nullptr, &replacement);
        if (refence == VK_SUCCESS) {
            vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            frame.inFlight = replacement;
        } else {
            vkFailed("vkCreateFence(replacing the fence of a refused submit; the next "
                     "frame's fence wait may not return)", refence);
        }
        return false;""", """        return false;""", 1)


def ce_refused_frame_keeps_the_acquire(s):
    """Review-1 F4, restored: the frame is refused after a successful acquire and the
    binary semaphore stays posted for the next frame's acquire to trip on. Only the
    drain is deleted, so only C4 can see it."""
    return s.replace("""    result = vkResetCommandBuffer(frame.commandBuffer, 0);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }""", """    result = vkResetCommandBuffer(frame.commandBuffer, 0);
    if (result != VK_SUCCESS) {
        return false;
    }""", 1)


arm_context("frame-submitted-unended", ce_frame_submitted_unended, "C2")
arm_context("fence-reset-in-begin-frame", ce_fence_reset_in_begin_frame, "C3")
arm_context("refused-submit-keeps-the-reset-fence", ce_refused_submit_keeps_the_reset_fence, "C3")
arm_context("refused-frame-keeps-the-acquire", ce_refused_frame_keeps_the_acquire, "C4")


def be_forget_stops_retiring(s):
    """The leak, restored exactly: review-25's fix kept, and the set it dropped never
    handed back. The cache is correct and the pool empties one set at a time. B12 only."""
    return s.replace("""            retireDescriptorSet(r, r->descs[i].set);
            r->descs[i] = r->descs.back();""",
                     """            r->descs[i] = r->descs.back();""", 1)


def be_set_freed_at_the_drop_site(s):
    """The one-line 'fix' that reads correctly and is invalid use: the set is freed at the
    moment the cache forgets it, which is a moment a recorded frame may still be binding
    it. B12 only."""
    return s.replace("            retireDescriptorSet(r, r->descs[i].set);",
                     "            vkFreeDescriptorSets(r->ctx.device, r->descPool, 1,"
                     " &r->descs[i].set);", 1)


def be_pool_not_freeable(s):
    """The create without the bit. `vkFreeDescriptorSets` is invalid use on a pool that
    did not ask for freeable sets, so the free is a second bug rather than a fix. B12
    only."""
    return s.replace("        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;\n",
                     "", 1)


def be_retired_sets_never_freed(s):
    """The bookkeeping without the release: sets are collected into the retire list and
    the list is drained into nothing. A vector is not a free. B12 only."""
    marker = "    for (size_t i = 0; i < r->retiringSets.size();) {"
    lo = s.find(marker)
    if lo < 0:
        return s
    hi = s.find("\n}\n", lo)
    if hi < 0:
        return s
    return s[:lo] + s[hi + 1:]


def be_set_freed_without_a_deadline(s):
    """The free is in the settle and the deadline is not: every retired set goes on the
    first frame that settles, whether or not the frame that bound it has finished. The
    call and its result are untouched, so only the boundary can be what reddens. B12
    only."""
    return s.replace("""        const RetiredSet retired = r->retiringSets[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }""", """        const RetiredSet retired = r->retiringSets[i];
        if (false) {
            i++;
            continue;
        }""", 1)


def be_set_free_result_discarded(s):
    """`vkFree*` is a mixed family — vkFreeMemory and vkFreeCommandBuffers are void and
    vkFreeDescriptorSets is not — and B5's verb list does not cover `Free`, so a
    discarded result here is invisible to every other check in the file. B12 only."""
    return s.replace("""        const VkResult freed = vkFreeDescriptorSets(r->ctx.device, r->descPool, 1, &set);
        if (freed != VK_SUCCESS) {""", """        vkFreeDescriptorSets(r->ctx.device, r->descPool, 1, &set);
        if (false) {""", 1)


arm_backend("forget-stops-retiring-the-set", be_forget_stops_retiring, "B12")
arm_backend("set-freed-at-the-drop-site", be_set_freed_at_the_drop_site, "B12")
arm_backend("pool-not-created-freeable", be_pool_not_freeable, "B12")
arm_backend("retired-sets-never-freed", be_retired_sets_never_freed, "B12")
arm_backend("set-freed-without-a-deadline", be_set_freed_without_a_deadline, "B12")
arm_backend("set-free-result-discarded", be_set_free_result_discarded, "B12")


def be_retire_records_nothing(s):
    """Review-9 F1 mutant 1, the one that matters: the drop site still calls the helper,
    the helper still names a deadline, and the record goes nowhere. The leak this batch
    exists to close is back at full size with every part of the shape intact. B12
    only."""
    lo = s.index("void retireDescriptorSet(")
    hi = s.index("\n}\n", lo)
    return s[:lo] + s[lo:hi].replace("r->retiringSets.push_back(retired);",
                                      "(void)retired;", 1) + s[hi:]


def be_settle_does_not_erase(s):
    """Review-9 F1 mutant 2: the free is real and the entry stays, so every frame after
    it frees the same handle again — and once the pool reissues that handle this is a
    free of a live descriptor set. The free, its result and the deadline all still
    read correctly. B12 only."""
    lo = s.index("void settleRetired(")
    hi = s.index("\n}\n", lo)
    return s[:lo] + s[lo:hi].replace(
        "r->retiringSets.erase(r->retiringSets.begin() + (long)i);", "i++;", 1) + s[hi:]


def be_deadline_one_frame_early(s):
    """Review-9 F1 mutant 3, the tidy-up that reads as an optimisation: settle one frame
    sooner. Retire in frame N, free at begin(N+1), and the fence waited there is frame
    N-1's — so the set goes back while the frame that bound it is still on the queue.
    B12 only."""
    lo = s.index("void retireDescriptorSet(")
    hi = s.index("\n}\n", lo)
    return s[:lo] + s[lo:hi].replace(
        "retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;",
        "retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight - 1;", 1) + s[hi:]


def be_retire_the_wrong_set(s):
    """Review-9 F1 mutant 4: the call is there and the deadline is right, but the set
    handed over is the last one in the cache instead of the one being dropped — a live
    pair's set, unless the dropped entry happened to sit last. B12 only."""
    return s.replace("retireDescriptorSet(r, r->descs[i].set);",
                     "retireDescriptorSet(r, r->descs.back().set);", 1)


arm_backend("retire-records-nothing", be_retire_records_nothing, "B12")
arm_backend("settle-does-not-erase", be_settle_does_not_erase, "B12")
arm_backend("deadline-one-frame-early", be_deadline_one_frame_early, "B12")
arm_backend("retire-the-wrong-set", be_retire_the_wrong_set, "B12")

checks = sum(r.checks for r in real)
arms = [("Linux frontend", FRONT_ARMS, FILES["Linux frontend"]),
        ("Linux backend", BACK_ARMS, FILES["Linux backend"]),
        ("Linux context", CTX_ARMS, FILES["Linux context"])]
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
