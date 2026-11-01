#!/usr/bin/env python3
"""Resolve a litellm alias to the model that actually answers, and say whether
two aliases are the same model.

WHY THIS EXISTS. On a shared litellm proxy an alias is a name chosen for
convenience, not a statement of fact: two aliases can be one upstream model,
and nothing in a chat response tells you which model answered — the response
echoes the alias you asked for. The loop's independence rule ("the adversarial
reviewer must not be the builder's own model") is therefore unsound if it
compares alias STRINGS, which is what it used to do (external review
2026-11-01, finding F3). This resolver is what it compares instead.

THE ORDER OF AUTHORITY, and why it is in this order:

  1. the live proxy's ``GET /model/info`` -> ``litellm_params.model``. This is
     the truth, and it also catches a stale static map.
  2. STATIC_UPSTREAMS below, used when the proxy does not answer. Every alias
     the loop ships with is listed, so the default configuration resolves
     without a network and the loop gains no new hard dependency on the info
     endpoint (which is a different permission from the chat endpoint).
  3. nothing. An alias that neither source resolves is reported UNKNOWN and the
     caller must treat it as NOT independent. "Could not tell" must not reach
     the same path as "proved different".

FAMILY is coarser than upstream on purpose. `deepseek/deepseek-flash` at
api.deepseek.com and `openai/deepseek-ai/deepseek-v4.1-flash` at
integrate.api.nvidia.com are two different upstream strings and ONE model
family: two hosts of the same weights are not two independent reviewers, which
is the only thing this guard is for.

USAGE
    model_identity.py upstream <alias> [<alias> ...]
    model_identity.py family   <alias> [<alias> ...]
    model_identity.py same     <alias> <alias>       # same|different|unknown
    model_identity.py report   [<alias> ...]         # resolved table (default: STATIC)
    model_identity.py selftest                       # asserts the known pairs

Exit status is 0 on a resolved answer and 1 on UNKNOWN, so a shell caller
cannot mistake "no answer" for "no problem". Aliases may be written with or
without their provider prefix (`litellm/qwen` and `qwen` are the same lookup).

ENVIRONMENT
    MDK_PI_MODELS_JSON   pi's models.json (default ~/.pi/agent/models.json)
    MDK_LITELLM_KEY      the proxy key (default: the file pi's apiKey points at)
    MDK_MODEL_INFO_URL   override the /model/info URL (default: baseUrl + /model/info)
    MDK_MODEL_INFO_TTL   cache seconds (default 600)
    The cache is per-uid and per-proxy (see _cache_path). A fixed filename in a
    world-writable directory would let another local account decide what the
    independence guard believes.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat as stat_module
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

# --- the static map ---------------------------------------------------------
# alias -> upstream string, transcribed from `GET /model/info` on the proxy in
# use, 2026-11-01. It is a FALLBACK and `selftest --live` is what proves it is
# still true; a drift is reported, not hidden.
STATIC_UPSTREAMS = {
    "qwen": "openai/qwen38-flash-next",
    "swarm-worker": "openai/qwen38-flash-next",
    "deepseek-v4.1-flash": "deepseek/deepseek-flash",
    "deepseek-v4.1-flash-nvidia": "openai/deepseek-ai/deepseek-v4.1-flash",
    "minimax-m3.1-flash-nvidia": "openai/minimaxai/minimax-m3.1-flash",
    "minimax-m3.1-nvidia": "openai/minimaxai/minimax-m3.1-flash",
    "glm-5.3-nvidia": "openai/z-ai/glm-5.3",
    "glm-5.3-flash-nvidia": "openai/z-ai/glm-5.3-flash",
    "kimi-k3-nvidia": "openai/moonshotai/kimi-k3",
    "luna": "openai/responses/gpt-6-luna",
    "luna-compaction": "openai/responses/gpt-6-luna",
    "sol": "openai/responses/gpt-6-sol",
    "grok": "xai/grok-4.6",
    "muse-spark": "meta/muse-spark-1.3",
    "muse-spark-contributor": "meta/muse-spark-1.3-contributor",
    "flux2-klein-4b": "openai/flux2-klein-4b",
    "kokoro": "openai/kokoro",
}

# upstream substring -> family. Ordered: the first hit wins, so the specific
# entries come before the general ones.
FAMILY_TOKENS = (
    ("qwen", "qwen"),
    ("deepseek", "deepseek"),
    ("minimax", "minimax"),
    ("glm", "glm"),
    ("kimi", "kimi"),
    ("gpt-6-luna", "luna"),
    ("gpt-6-sol", "sol"),
    ("grok", "grok"),
    ("muse-spark", "muse"),
    ("flux", "image"),
    ("kokoro", "tts"),
)


def bare(alias: str) -> str:
    """`litellm/qwen` and `qwen` are one lookup."""
    return alias.split("/")[-1].strip()


def family_of(upstream: str) -> str:
    low = upstream.lower()
    for token, family in FAMILY_TOKENS:
        if token in low:
            return family
    return ""


def _models_json() -> dict:
    path = os.environ.get("MDK_PI_MODELS_JSON") or (Path.home() / ".pi/agent/models.json")
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return {}


def _proxy_config() -> tuple[str, str]:
    """(model_info_url, key). Empty strings when they cannot be determined."""
    url = os.environ.get("MDK_MODEL_INFO_URL", "")
    key = os.environ.get("MDK_LITELLM_KEY", "")
    cfg = (_models_json().get("providers") or {}).get("litellm") or {}
    if not url:
        base = str(cfg.get("baseUrl") or "").rstrip("/")
        if base:
            url = f"{base}/model/info"
    if not key:
        raw = str(cfg.get("apiKey") or "")
        # pi stores "!cat <path>" for a key it must not hold. Read the file;
        # never run the command.
        if raw.startswith("!"):
            cmd = raw[1:].strip()
            if cmd.startswith("cat "):
                target = cmd[4:].strip().replace("~", str(Path.home()))
                try:
                    key = Path(target).read_text().strip()
                except OSError:
                    key = ""
        else:
            key = raw
    return url, key


def live_table() -> dict:
    """alias -> upstream from the proxy, or {} when it will not answer.

    Cached for MDK_MODEL_INFO_TTL seconds: the loop asks for two or three
    aliases per iteration and the answer does not change minute to minute.
    """
    url, key = _proxy_config()
    if not url:
        return {}
    try:
        ttl = int(os.environ.get("MDK_MODEL_INFO_TTL", "600"))
    except ValueError:
        ttl = 600
    cache = _cache_path(url, key)
    if ttl > 0 and cache.is_file() and time.time() - cache.stat().st_mtime < ttl:
        cached = _read_cache(cache)
        if cached is not None:
            return cached
        # A cache we will not trust is not a cache: fall through and ask the
        # proxy, which is the truth, rather than falling back to the static map
        # because a file was poisoned.
    try:
        req = urllib.request.Request(url, headers={"Authorization": f"Bearer {key}"})
        with urllib.request.urlopen(req, timeout=20) as reply:
            payload = json.loads(reply.read().decode())
    except Exception:
        # Any failure at all — unreachable, 403, a body that is not JSON —
        # means "no live answer", and the caller falls back to the static map.
        return {}
    table: dict[str, str] = {}
    for entry in payload.get("data") or []:
        name = entry.get("model_name")
        upstream = (entry.get("litellm_params") or {}).get("model")
        if name and upstream:
            table[bare(str(name))] = str(upstream)
    if table:
        _write_cache(cache, table)
    return table


def _cache_path(url: str, key: str) -> Path:
    """Where the live table is cached: per-uid, and keyed on the proxy it came from.

    This is a trust boundary, not a nicety (external review 2026-11-01, finding
    L4). The old path was ``$TMPDIR/mdk-model-info.json``: a fixed name in a
    world-writable directory, with no ownership check, not keyed on the proxy or
    the key, and then trusted for the whole TTL as *the* answer to "what model is
    this alias". Another local account that created that file first could decide
    what the independence guard believes — either refuse every reviewer, or let
    the builder's own family through. The uid keeps other accounts out; the
    digest of url+key means a proxy or credential change cannot keep reading a
    table fetched from the old one.
    """
    digest = hashlib.sha256(f"{url}\n{key}".encode()).hexdigest()[:16]
    return Path(os.environ.get("TMPDIR", "/tmp")) / f"mdk-model-info-{os.getuid()}-{digest}.json"


def _read_cache(path: Path) -> dict | None:
    """The cached table, or None unless it is ours, well-shaped and parseable.

    Ownership is checked, never assumed: /tmp is sticky precisely so that a file
    found there may belong to somebody else, and a table somebody else wrote is
    not evidence about the proxy.

    The check runs on the descriptor that is then READ, not on a separate `stat`
    of the path (external review 2026-11-01, run-20261101-025705/review-3.md F5).
    `path.stat()` followed by `path.read_text()` is a TOCTOU pair, and both of
    them follow a symlink, so a planted link at the cache path would have had its
    TARGET inspected for ownership and its BODY read as the answer — which is the
    whole attack the per-uid path exists to stop, minus the need to guess the
    digest. `O_NOFOLLOW` refuses the link and `fstat` describes the exact open
    file, so there is nothing left to swap between the check and the read.

    `O_NONBLOCK` is there because the check above is a check on the file it
    opened, and an open that never returns never gets to check anything (external
    review 2026-11-01, run-20261101-062333/review-1.md AR-3). The cache path is
    under a world-writable `$TMPDIR`, and a FIFO left at it makes a plain
    `O_RDONLY` open wait for a writer forever: the ownership gate is then not
    merely bypassed, it is unreachable, and the caller (`model_upstream`, on the
    review-decision path) has no timeout of its own. On Linux `O_NONBLOCK` is
    inert for a regular file and turns a FIFO open into an immediate `ENXIO`, so
    the unusable cache is refused and the live fetch answers, which is the
    behaviour a cache must have: a cache that can hang the caller is worse than
    no cache.
    """
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
    except OSError:
        return None
    try:
        info = os.fstat(fd)
        if not stat_module.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            return None
        with os.fdopen(fd, "r") as handle:
            fd = -1
            payload = handle.read()
    except (OSError, UnicodeDecodeError):
        return None
    finally:
        if fd >= 0:
            os.close(fd)
    try:
        table = json.loads(payload)
    except ValueError:
        return None
    if not isinstance(table, dict):
        return None
    if not all(isinstance(k, str) and isinstance(v, str) and k and v for k, v in table.items()):
        return None
    return table


def _write_cache(path: Path, table: dict) -> None:
    """Best effort at 0600, and never fatal: the live fetch already answered, the
    cache only saves the next round trip.

    `O_NOFOLLOW` for the same reason as the reader: without it, a symlink left at
    the cache path is opened THROUGH, and `O_TRUNC` then destroys whatever the link
    points at — a cache write must only ever write the cache file. `O_NONBLOCK`
    for the reader's reason too, and it bites harder here: opening a FIFO for
    WRITING blocks until a reader appears, so without it a planted FIFO stops the
    loop in the middle of a cache write rather than at the read.
    """
    try:
        fd = os.open(
            path,
            os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0),
            0o600,
        )
        # fchmod, not just the open mode: O_CREAT does not touch the mode of a
        # file that already existed, and a file someone else left behind would
        # keep whatever permissions it came with.
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w") as handle:
            json.dump(table, handle)
    except OSError:
        pass


_CHILD_PROBE = """
import pathlib, sys
sys.path.insert(0, sys.argv[1])
import model_identity as m
path = pathlib.Path(sys.argv[3])
if sys.argv[2] == "read":
    print("NONE" if m._read_cache(path) is None else "READ")
else:
    m._write_cache(path, {"planted": "openai/not-the-proxy"})
    print("WROTE")
"""


def _probe_in_child(path: Path, mode: str, timeout: float = 10.0) -> str:
    """Run _read_cache/_write_cache on `path` in a child that can be abandoned.

    A blocking open is the exact defect being pinned, so the claim cannot be
    checked in this process: if the guard regressed, the selftest would HANG
    instead of failing, and a suite that hangs is indistinguishable from a suite
    that was never run. The child prints the outcome; the parent waits with a
    deadline and reports HUNG, which turns the regression into a FAIL line.
    """
    try:
        out = subprocess.run(
            [sys.executable, "-c", _CHILD_PROBE, str(Path(__file__).resolve().parent), mode, str(path)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return "HUNG"
    return out.stdout.strip() or f"ERROR rc={out.returncode}"


def _selftest_cache() -> bool:
    """Prove the cache refuses what it must.

    The uid-in-path and per-proxy keying are checkable from the path string. The
    `st_uid` refusal used to be written off as uncheckable ("making a file here
    belong to another account needs root"), which is true of the FILE and beside
    the point: the guard is a comparison against `os.getuid()`, and that is a
    function, so the refusal is asserted by pretending to be the other account
    (external review 2026-11-01, run-20261101-025705/review-3.md F5). Before this
    the guard had no test at all and deleting it left the whole suite green, which
    is the same defect the loop keeps filing against itself: a defence whose
    failure is invisible is not a defence.
    """
    tmp = tempfile.mkdtemp(prefix="mdk-identity-selftest-")
    old = os.environ.get("TMPDIR")
    os.environ["TMPDIR"] = tmp
    try:
        path = _cache_path("http://self-test.invalid/model/info", "k")
        checks = []
        path.write_text('{"qwen": 42}')
        checks.append(("a non-string value is refused", _read_cache(path) is None))
        path.write_text("not json at all")
        checks.append(("unparseable content is refused", _read_cache(path) is None))
        path.write_text('["a", "b"]')
        checks.append(("a non-object is refused", _read_cache(path) is None))
        _write_cache(path, {"qwen": "openai/qwen38-flash-next"})
        checks.append(("a table we wrote is read back", _read_cache(path) == {"qwen": "openai/qwen38-flash-next"}))
        checks.append(("the cache file is created 0600", (path.stat().st_mode & 0o777) == 0o600))
        checks.append(("the path carries the uid", f"-{os.getuid()}-" in path.name))
        checks.append(("the path changes with the proxy", _cache_path("http://a", "k") != _cache_path("http://b", "k")))
        checks.append(("the path changes with the key", _cache_path("http://a", "k1") != _cache_path("http://a", "k2")))
        # The ownership guard, pinned. One uid over is a different account, and a
        # table from a different account is not evidence about the proxy.
        real_getuid = os.getuid
        ours = _read_cache(path)
        try:
            os.getuid = lambda: real_getuid() + 1
            checks.append(("a table written by another uid is REFUSED", _read_cache(path) is None))
            checks.append(("and the refusal is not just a broken read", ours is not None))
        finally:
            os.getuid = real_getuid
        checks.append(("the same file reads back when the uid matches again",
                       _read_cache(path) == {"qwen": "openai/qwen38-flash-next"}))
        # A symlink at the cache path is not the cache. Read: it is not what the
        # digest named. Written: O_TRUNC would destroy the link's target.
        victim = Path(tmp) / "someone-elses-file.json"
        victim.write_text('{"legit": "openai/qwen38-flash-next"}')
        link = Path(tmp) / f"mdk-model-info-{os.getuid()}-ffffffffffffffff.json"
        try:
            link.symlink_to(victim)
        except OSError:
            checks.append(("symlinks cannot be created on this filesystem", True))
        else:
            checks.append(("a symlink at the cache path is refused for reading",
                           _read_cache(link) is None))
            _write_cache(link, {"planted": "openai/not-the-proxy"})
            checks.append(("a symlink at the cache path is NOT written through",
                           victim.read_text() == '{"legit": "openai/qwen38-flash-next"}'))
            checks.append(("and the link itself is left alone", link.is_symlink()))
        # A directory at the path is not a cache file either; O_RDONLY would open it.
        checks.append(("a directory at the cache path is refused", _read_cache(Path(tmp)) is None))
        # A FIFO at the path is not a cache file either, and it is the one that can
        # take the caller down with it: a plain open blocks until somebody opens the
        # other end, so the ownership check behind the open never runs (AR-3). Both
        # directions are pinned, because the writer blocks on a FIFO too — on a
        # missing READER — and the check runs in a child so a regression prints a
        # FAIL instead of hanging the suite.
        fifo = Path(tmp) / f"mdk-model-info-{os.getuid()}-eeeeeeeeeeeeeeee.json"
        try:
            os.mkfifo(fifo)
        except (OSError, AttributeError):
            checks.append(("FIFOs cannot be created on this filesystem", True))
        else:
            checks.append(("a FIFO at the cache path is REFUSED on read, not blocked",
                           _probe_in_child(fifo, "read") == "NONE"))
            checks.append(("a FIFO at the cache path does not block the writer",
                           _probe_in_child(fifo, "write") == "WROTE"))
            checks.append(("and the FIFO is still there, untouched, afterwards", fifo.is_fifo()))
        for label, ok in checks:
            print(f"{'PASS' if ok else 'FAIL'} cache: {label}")
        return all(ok for _, ok in checks)
    finally:
        if old is None:
            os.environ.pop("TMPDIR", None)
        else:
            os.environ["TMPDIR"] = old
        shutil.rmtree(tmp, ignore_errors=True)


def resolve(alias: str, table: dict | None = None) -> tuple[str, str]:
    """(upstream, family) for one alias; ("", "") when it is unknown."""
    name = bare(alias)
    upstream = ""
    if table is None:
        table = live_table() or dict(STATIC_UPSTREAMS)
    upstream = (table or {}).get(name) or STATIC_UPSTREAMS.get(name) or ""
    return upstream, family_of(upstream)


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if a != "--live"]
    live = "--live" in argv[1:]
    if not args:
        print(__doc__.strip().splitlines()[-1])
        return 2
    mode, rest = args[0], args[1:]
    table = live_table() if live else None
    if live and not table:
        print("note: --live asked for and the proxy did not answer; static map in use",
              file=sys.stderr)

    if mode in ("upstream", "family"):
        rc = 0
        for alias in rest:
            upstream, family = resolve(alias, table)
            value = upstream if mode == "upstream" else family
            print(value)
            if not value:
                rc = 1
        return rc

    if mode == "same":
        if len(rest) != 2:
            print("same takes exactly two aliases", file=sys.stderr)
            return 2
        ua, fa = resolve(rest[0], table)
        ub, fb = resolve(rest[1], table)
        if not ua or not ub:
            print("unknown")
            return 1
        if ua == ub or (fa and fa == fb):
            print("same")
            return 0
        print("different")
        return 0

    if mode == "report":
        names = [bare(a) for a in rest] or sorted(STATIC_UPSTREAMS)
        for name in names:
            upstream, family = resolve(name, table)
            source = "live" if (table or {}).get(name) else ("static" if upstream else "UNKNOWN")
            print(f"{name:32s} {upstream or '-':46s} {family or '-':12s} {source}")
        return 0

    if mode == "selftest":
        return selftest(table)

    print(f"unknown mode {mode!r}", file=sys.stderr)
    return 2


def selftest(table: dict | None) -> int:
    """Assert what the guard needs to be true. Each line is a claim a reader
    can check against the proxy, not a tautology about this file."""
    checks = [
        # ("label", callable, expected)
        ("qwen and swarm-worker are ONE upstream",
         lambda: resolve("qwen")[0] == resolve("swarm-worker")[0], True),
        ("qwen and swarm-worker are ONE family",
         lambda: resolve("qwen")[1] == resolve("swarm-worker")[1], True),
        ("luna and luna-compaction are ONE upstream under two aliases",
         lambda: resolve("luna")[0] == resolve("luna-compaction")[0], True),
        ("the two DeepSeek aliases are different upstream strings",
         lambda: resolve("deepseek-v4.1-flash")[0] != resolve("deepseek-v4.1-flash-nvidia")[0],
         True),
        ("...and still ONE family, so neither may review the other",
         lambda: resolve("deepseek-v4.1-flash")[1] == resolve("deepseek-v4.1-flash-nvidia")[1],
         True),
        ("the shipped builder and the shipped reviewer are different families",
         lambda: resolve("qwen")[1] != resolve("deepseek-v4.1-flash")[1], True),
        ("an alias nobody knows resolves to nothing, not to a guess",
         lambda: resolve("litellm/not-a-model") == ("", ""), True),
        ("the provider prefix does not change the answer",
         lambda: resolve("litellm/qwen") == resolve("qwen"), True),
    ]
    if table:
        checks.append((
            "the static map agrees with the live proxy for every alias it lists",
            lambda: all(STATIC_UPSTREAMS[a] == table.get(a) for a in STATIC_UPSTREAMS
                        if a in table) and any(a in table for a in STATIC_UPSTREAMS),
            True))
    else:
        checks.append((
            "live proxy check skipped (no answer; static map only)", True, True))

    failures = 0
    for label, got, want in checks:
        try:
            value = got() if callable(got) else got
        except Exception as exc:  # a broken resolver must fail the selftest
            value, failures = f"raised {exc}", failures + 1
        ok = value == want
        failures += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'} {label}" + ("" if ok else f"  (got {value!r})"))
    if not _selftest_cache():
        failures += 1
    print("MODEL IDENTITY: " + ("SELFTEST CLEAN" if failures == 0 else f"{failures} FAILED"))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
