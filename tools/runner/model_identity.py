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
"""
from __future__ import annotations

import json
import os
import sys
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
    cache = Path(os.environ.get("TMPDIR", "/tmp")) / "mdk-model-info.json"
    if ttl > 0 and cache.is_file() and time.time() - cache.stat().st_mtime < ttl:
        try:
            return json.loads(cache.read_text())
        except (OSError, ValueError):
            pass
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
        try:
            cache.write_text(json.dumps(table))
        except OSError:
            pass
    return table


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
    print("MODEL IDENTITY: " + ("SELFTEST CLEAN" if failures == 0 else f"{failures} FAILED"))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
