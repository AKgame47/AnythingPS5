# `pnr-launcher ask` (slice D1) - EXPERIMENTAL

A natural-language front end for the launcher. You type a sentence, a small
local model turns it into one launcher command, PNR checks that command
itself, and then either runs it (read-only commands) or asks you first.

Status: **Experimental.** Tested with a fake model executable and with one
host's real binary on a handful of prompts. No accuracy benchmark exists and
none is claimed. The confidence threshold is **uncalibrated**.

## Setup (you do this, PNR downloads nothing)

    export PNR_NEEDLE=/path/to/needle     # your own copy of the binary
    pnr-launcher ask "what games do I have"
    pnr-launcher ask "..." --threshold 0.9   # flag after the sentence; default 0.8

Without `PNR_NEEDLE`: "ask is not configured", exit code 2.

## Licence evidence (checked 2026-11-14, not legal advice)

- Model card `Cactus-Compute/needle2` on Hugging Face: Apache-2.0
  (API card data and tag, plus a LICENSE file in the repo).
- GitHub `cactus-compute/needle`: Apache-2.0 LICENSE.
- PyPI `cactus-needle` 3.2.10: Apache-2.0.
- The Hugging Face repo ships a `linux-x86_64/needle` executable
  (14,888,896 bytes, model baked in). It ran on the test host using about
  22 MB RAM and about 0.7 s per prompt.

## The model runs inside PNR's sandbox (slice D2)

By default the model program runs in the same namespace + pivot_root sandbox
as `launch --sandbox` (see docs/sandbox.md): no network (loopback only), a
private PID namespace, system directories read-only, and a seccomp denylist.
It sees only a private 0700 temp directory holding a COPY of the executable
and the generated tools file. Your home directory and library are not visible.
The temp directory is removed afterwards.

- No silent fallback: if the sandbox is unusable the model is not run, and
  the output says so. `--no-model-sandbox` is the explicit opt-out and the
  output then says "NOT sandboxed".
- Tested with a probe script (user file hidden, /etc read-only, tools file
  readable, only `lo`, PID 1) and a control run without the sandbox that does
  see the user file. The real binary also ran sandboxed on the test host.
- Still PARTIAL, like the game sandbox: it is namespaces plus a denylist, not
  a proof of escape-proofing, and it needs unprivileged user namespaces.
- On a timeout the sandbox parent is killed; PNR removes its leftover empty
  mountpoint dir in /tmp on a best-effort basis.

## Why external executable only

PNR is GPL-2.0. Apache-2.0 is not compatible with GPLv2-only. To stay
conservative, PNR never vendors, links or ships the model or its library. It
only starts a program you provide, over a plain process boundary, passing a
tool list and a prompt and reading JSON back.

## What PNR checks itself

The model output is untrusted. `interpret()` rejects: bad JSON, a failed
run, negation flagged, ungrounded flagged, more than one call, no call, an
unknown tool, confidence below threshold, and bad arguments. Arguments must
have exactly the expected keys, be strings, have no leading `-`, no control
characters; game ids, sources and packages must exist; a path must be an
existing file; search text is at most 120 characters.

The model runs with no shell, a cleared environment (PATH and LANG only),
stdin from /dev/null, CPU and memory limits, a 30 s wall timeout and a
1 MiB output cap. The prompt is 1 to 500 printable characters.

## Command table

| Command | How `ask` treats it |
|---|---|
| list, show, compat show, history, store search, store show, research inspect, sandbox probe, doctor, suggest_manifest (10 read-only tools; suggest_manifest never gets `--out`, see docs/manifest-suggest.md) | runs directly |
| launch, store get, store install, import, remove (5 tools) | always asks y/N on a terminal; never runs non-interactively |

`remove` through `ask` is a dry run: `--yes` is never passed.

Never reachable through the model: `psn_*`, `set-token`, `source add`,
`compat set`, `--allow-flagged`, `--yes`, `--force`, `research fetch`.

## Honest accuracy notes

Observed with the real binary:

- "find me something with racing" picked the wrong tool (list_games, 0.53,
  on an early 3-tool table). Confidence is not a safety measure.
- "launch st-game; rm -rf /" once produced launch_game with game id
  `rm -rf /` at 0.30. PNR refuses it because no such game exists, and it
  would never reach a shell anyway.
- The model is sometimes confidently wrong. Check the printed
  "parsed command" line before confirming.

## Limits

- The model is sandboxed by default (D2) but the sandbox is partial; with `--no-model-sandbox` it is not.
- One host, one binary version, few prompts.
- English only was tried.

## Real captured output

Library holds the PNR demo game. Real Needle binary, sandboxed, run on 2026-11-14.

```
$ pnr-launcher ask "what games do I have"
PNR Demo Game  v0.1.0  [Untested]  runnable  id=demo-game
model (experimental, uncalibrated, sandboxed): list_games, confidence 0.96, threshold 0.80
parsed command: pnr-launcher list

$ pnr-launcher ask "find me something with racing"
model (experimental, uncalibrated, sandboxed): store_search, confidence 0.07, threshold 0.80
NOT RUN: confidence below threshold: confidence is below your threshold
closest guess (not run): pnr-launcher store search racing

$ pnr-launcher ask "what games do I have" --no-model-sandbox
PNR Demo Game  v0.1.0  [Untested]  runnable  id=demo-game
model (experimental, uncalibrated, NOT sandboxed): list_games, confidence 0.96, threshold 0.80
parsed command: pnr-launcher list
```

More real runs (launch refused non-interactively, TTY decline, injection
attempt, unknown game) were captured in D1; the verdicts are unchanged.
