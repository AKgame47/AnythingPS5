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
| list, show, compat show, history, store search, store show, research inspect, sandbox probe, doctor (9 read-only tools) | runs directly |
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

- Needle runs outside the sandbox in D1.
- One host, one binary version, few prompts.
- English only was tried.

## Real captured output

Library holds the PNR demo game. Real Needle binary, run on 2026-11-14.

```
$ pnr-launcher ask "what games do I have"
PNR Demo Game  v0.1.0  [Untested]  runnable  id=demo-game
model (experimental, uncalibrated): list_games, confidence 0.96, threshold 0.80
parsed command: pnr-launcher list

$ pnr-launcher ask "start the game demo-game"
model (experimental, uncalibrated): launch_game, confidence 0.94, threshold 0.80
parsed command: pnr-launcher launch demo-game
NOT RUN: this command changes things or starts a game, and confirmation needs an interactive terminal. Run it yourself if it is what you meant.

$ pnr-launcher ask "find me something with racing"
model (experimental, uncalibrated): store_search, confidence 0.07, threshold 0.80
NOT RUN: confidence below threshold: confidence is below your threshold
closest guess (not run): pnr-launcher store search racing

$ pnr-launcher ask "do not launch demo-game"
model (experimental, uncalibrated): no command, confidence 0.76, threshold 0.80
NOT RUN: negation detected: the sentence contains a negation, so PNR will not guess the opposite

$ pnr-launcher ask "launch demo-game; rm -rf /"
model (experimental, uncalibrated): no command, confidence 0.07, threshold 0.80
NOT RUN: more than one call: the model chose several commands; ask for one thing at a time

$ pnr-launcher ask "launch the game called nonexistent"
model (experimental, uncalibrated): launch_game, confidence 0.97, threshold 0.80
NOT RUN: arguments rejected: no installed game with that exact id

$ (in a terminal) pnr-launcher ask "start the game demo-game"  -> answer n
n
run this? pnr-launcher launch demo-game  [y/N] model (experimental, uncalibrated): launch_game, confidence 0.94, threshold 0.80
parsed command: pnr-launcher launch demo-game
not run (you declined)
```
