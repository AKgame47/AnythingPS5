# PNR research tools (slice C1)

Status: **Implemented + Tested here (Linux x86-64).** Nothing here touches Sony content, DRM or authentication.

Three commands under `pnr-launcher research`. All data is local, in `<store dir>/research/`.

## Research log

```
research log add <text>
research log list [--limit N] [--json]
research log verify
```

An append-only text file (`log.jsonl`). Every entry carries the hash of the previous one, so edits, deleted middle entries and reordering are **detected** by `log verify`, and PNR refuses to append to a broken chain.

Limits, stated honestly:
- It is **not tamper-proof**. Anyone who can rewrite the whole file can recompute the chain.
- Dropping the **newest** entries cannot be detected.
- Times come from your system clock.
- Concurrent writers are safe (file lock). Tested with 4 processes.

Entry kinds: `note` (you), `fetch`, `fetch-failed`, `inspect` (with `--log`).

## Fetch

```
research fetch <https-url> <out-file> --rights <statement> [--sha256 HEX] [--force]
```

One URL you supply, one file. There are **no default sources**.

Rules:
- https only, port 443, no userinfo, no `#fragment`.
- Only the URL's own host. **Redirects are refused**, not followed.
- Resumable: data goes to `<out>.part`; a later run resumes with a Range request (and `If-Range` when the server gave an ETag).
- A rights statement is **required**. It is recorded in the log. PNR cannot verify it.
- `--sha256` is enforced: a mismatch deletes the file and fails.
- Without `--sha256` the result says **integrity UNVERIFIED**: the hash shown is only what arrived. The log says so too.
- If the server declares no size, that is recorded (a truncated file would only be caught by `--sha256`).
- Limits: `--max-bytes` default 2 GiB, free-space check, retries with backoff on timeouts, 429 and 5xx.
- **Sony hosts are refused** (playstation.com, playstation.net, sony.com, sonyentertainmentnetwork.com, sie.com, scei.co.jp and their subdomains). PNR has no authorized way to download Sony content. The refusal is logged.
- An existing output file is not replaced without `--force`.

Not tested: a real internet host. Tests use an in-process fake that speaks Range/If-Range. The libcurl backend is the same one the store uses (tested against a local server in slice A2).

## Inspect

```
research inspect <file> [--log] [--json]
```

Read-only. Always reports size, SHA-256, first bytes and entropy. Then by format:

| Format | What you get |
| --- | --- |
| `.pnrgame` (tar) | Full read-only validation: structure, manifest, every SHA-256, x86-64 ELF entry. Nothing is extracted. |
| x86-64 ELF | class, type, machine, entry, segments, "PNR can launch natively" |
| JSON shaped like a store index | package count. Signatures are **not** validated here (use `store refresh`) |
| gzip, zip, sqlite, png | recognised, not opened |
| Sony package container (magic `7f 43 4e 54`) | recognised **by magic only**. Not parsed, not decrypted, not installed, not run |
| anything else | `unknown` |

High entropy (above 7.5 bits per byte) adds a note that the data may be compressed or encrypted. That is an indication, not proof. PNR does not try to decrypt anything.

## Real captured output

```
$ pnr-launcher research inspect demo.pnrgame --log
file:    demo.pnrgame
kind:    pnrgame
size:    66560 bytes
sha256:  d11a73e3cd4a1a8776bfe4940779eca5ff1e2e8762f4c1fd80dd829d4734044a
magic:   6d616e6966657374
entropy: 5.21 bits/byte (first 1 MiB)
  id: demo-game
  title: PNR Demo Game
  version: 0.1.0
  executable: bin/game
  arch: x86-64
  validation: structure, manifest, every SHA-256 and the x86-64 ELF entry point passed (read-only; nothing extracted)
logged in the research log

$ pnr-launcher research log add "checked demo package, looks like ours"
logged #2 (2026-11-13T21:48:09Z)

$ pnr-launcher research inspect some.pkg
file:    some.pkg
kind:    sony-package-container
size:    6 bytes
sha256:  41e1626e29ca418f1c7458db830a89cee4424c4ca495b68b913760b81d777f27
magic:   7f434e540001
entropy: 2.58 bits/byte (first 1 MiB)
note: This looks like a Sony package container (magic 7f 43 4e 54). PNR does not parse, decrypt, install or run Sony packages. Only its magic, size, SHA-256 and entropy were computed; the container was not parsed.

$ pnr-launcher research fetch https://store.playstation.com/x out --rights mine
research: PNR does not fetch from Sony hosts (no authorized download mechanism exists)

$ pnr-launcher research log list
#1 2026-11-13T21:48:09Z inspect demo.pnrgame
    kind: pnrgame; size 66560
    sha256 d11a73e3cd4a1a8776bfe4940779eca5ff1e2e8762f4c1fd80dd829d4734044a
#2 2026-11-13T21:48:09Z note
    checked demo package, looks like ours
#3 2026-11-13T21:48:09Z fetch-failed https://store.playstation.com/x
    refused or failed: PNR does not fetch from Sony hosts (no authorized download mechanism exists)

$ pnr-launcher research log verify
research log OK: chain intact: 3 entries. This detects edits, deletions in the middle and reordering; it cannot detect removal of the newest entries, or a rewrite of the whole file.
```

## Tests

`tests/research_tests.cpp` (57 checks) plus the research part of `pnr_store_workflow`:
- Log: round-trip with quotes, chain links, edit, deleted-middle, reorder and garbage detected, append refused on a broken chain, the newest-entry limit stated, 4 concurrent writers.
- Fetch: verified download, unverified marking, wrong hash deletes, resume after interruption with the Range header, retry on drop and 503, redirect refused, 404 not retried, oversend refused, `--max-bytes`, no-declared-size flag, http/userinfo/port/fragment/Sony/no-rights/bad-hash refusals, nothing sent on refusal.
- Inspect: real ELF, Sony magic not opened, valid and corrupted `.pnrgame`, store-index JSON, broken JSON, entropy, missing file, directory.
