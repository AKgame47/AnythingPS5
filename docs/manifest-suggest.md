# `pnr-launcher manifest suggest` (slice D2b) - EXPERIMENTAL

Prints a DRAFT `manifest.pnr` for a directory you point it at. It is
deterministic code. No model is involved in producing the draft.

    pnr-launcher manifest suggest <dir> [--out FILE]

## What it does and does not do

- Read-only. It never installs, imports, runs or changes anything.
- Without `--out` it writes nothing. With `--out FILE` it writes the draft to
  a NEW file only: it refuses to overwrite and never follows a symlink.
- It looks at up to 5000 entries, 4 levels deep, never follows symlinks, and
  only recognises native x86-64 Linux ELF executables. It ignores `.so` files,
  non-x86-64 ELF, Windows `.exe`, and any other container (including
  Sony-looking files, which are never opened or parsed).
- Every field is labelled: `read` (taken from a file), `guessed`, or
  `NOT INFERRED`. `id` and `title` are guessed from the directory name,
  `executable` is guessed (with several candidates it picks by name and says
  to check), `version` is never invented (placeholder 0.1.0, marked NOT
  INFERRED), `arch` is read from the ELF header.
- The draft is only `manifest.pnr`. It does not build `checksums.sha256` or
  the `.pnrgame` archive. You review it, then package it yourself.
- Exit codes: 0 draft printed, 1 no usable executable found, 2 error.

## From `ask`

`ask` has one more read-only tool, `suggest_manifest(dir)`, which routes a
sentence to this command with a fixed argv: the model only picks the
directory. `--out` is never reachable through the model, so `ask` cannot
write a file. The directory must exist. The model sometimes errs (see
docs/ask.md); the draft is always shown for you to review.

## Real captured output

Directory with one real x86-64 ELF at `bin/racer`. Real run, 2026-11-14.

```
$ pnr-launcher manifest suggest /tmp/sugdemo/space-racer
# DRAFT manifest.pnr, suggested by PNR (Experimental, deterministic, no model).
# Nothing was installed or changed. Review every line before building a package.
# id: guessed - from the directory name; must be a unique directory-safe name
id = space-racer
# title: guessed - from the directory name
title = Space Racer
# version: NOT INFERRED - placeholder: nothing on disk states a version; set it yourself
version = 0.1.0
# executable: guessed - the only x86-64 ELF executable found
executable = bin/racer
# arch: read - the chosen file's ELF header says x86-64
arch = x86-64
# Not covered: checksums.sha256 and the .pnrgame archive itself (build them with PNR's packaging tools).
(nothing written; use --out FILE to save this draft)
(exit 0)

$ pnr-launcher manifest suggest /tmp/sugdemo/space-racer --out /tmp/sugdemo/draft.pnr  (run twice)
written to /tmp/sugdemo/draft.pnr (a draft: guessed fields still need your review)
NOT WRITTEN: cannot create /tmp/sugdemo/draft.pnr (it must not exist yet)

$ pnr-launcher manifest suggest /tmp/sugdemo   (no ELF at top; nested one found)
# DRAFT manifest.pnr, suggested by PNR (Experimental, deterministic, no model).
# Nothing was installed or changed. Review every line before building a package.
# id: guessed - from the directory name; must be a unique directory-safe name
id = sugdemo
# title: guessed - from the directory name
title = Sugdemo
# version: NOT INFERRED - placeholder: nothing on disk states a version; set it yourself
version = 0.1.0
# executable: guessed - the only x86-64 ELF executable found
```

Through `ask` with the real Needle binary (sandboxed):

```
$ pnr-launcher ask "make a draft manifest for the directory /tmp/sugdemo/space-racer"
(prints the same draft as above, then:)
model (experimental, uncalibrated, sandboxed): suggest_manifest, confidence 0.93, threshold 0.80
parsed command: pnr-launcher manifest suggest /tmp/sugdemo/space-racer

$ pnr-launcher ask "write a manifest for /tmp/sugdemo/nonexistent"
model (experimental, uncalibrated, sandboxed): suggest_manifest, confidence 0.90, threshold 0.80
NOT RUN: arguments rejected: no such directory

$ pnr-launcher ask "suggest a manifest for /tmp/sugdemo/space-racer and install it"
model (experimental, uncalibrated, sandboxed): no command, confidence 0.14, threshold 0.80
NOT RUN: more than one call: the model chose several commands; ask for one thing at a time
```

## Tested

22 unit checks in tests/suggest_tests.cpp (single and several candidates,
guess labelling, draft parses with the manifest rules, read-only, `--out`
no-overwrite and no-symlink, ignored file kinds, depth cap, bad usage) and a
table check that no `ask` argv contains `--out`. No benchmark, no claim about
accuracy beyond that: it is a heuristic, not a build tool.
