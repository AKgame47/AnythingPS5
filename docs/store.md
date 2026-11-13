# PNR Unofficial Store: core and downloads (slices A1, A2)

Status: **Implemented + Tested (local fakes only).** Sources, index
validation, refresh, search, downloads and install work. **Tested only
against fakes and a local Range server. Never run against a real source.**
There is no update path yet.

## What it is

A content-neutral mechanism, like a package manager's custom repositories.

- The store starts with **no sources and no default source.**
- You add every source yourself: an https index URL plus a rights statement.
- **You are responsible for the sources you add.** The rights statement is what *you* know or the source claims. PNR **cannot verify** it and says so wherever it is shown.
- PNR has no source-specific code and ships no source list. It will not be pointed at piracy or repack sites by PNR, and its docs and examples never name one.

## Commands

```
pnr-launcher store source add <id> <https-index-url> --rights "<statement>"
pnr-launcher store source remove <id> [--yes]      (dry run without --yes)
pnr-launcher store source list [--json]
pnr-launcher store refresh [id] [--json]           (the only command that uses the network)
pnr-launcher store search [text] [--source id] [--json]
pnr-launcher store show <source> <package> [--json]
```

Data lives in `<library dir>/store/` (override: `PNR_STORE_DIR`): `sources.json` and one cached index per source.

## Index format `pnr-store-index-1`

```json
{
  "format": "pnr-store-index-1",
  "name": "My games",
  "rights": "Games by the author, shared under their licences",
  "packages": [
    { "id": "alpha", "name": "Alpha", "version": "1.0", "license": "MIT",
      "kind": "pnr-package", "size": 123456, "sha256": "<64 lowercase hex>",
      "url": "https://same-host-as-the-index/alpha.pnrgame",
      "description": "optional", "category": "optional", "rights": "optional per-package statement",
      "experimental": false }
  ]
}
```

An index is rejected **whole** (and the previous cached copy is kept) if anything is wrong:

- wrong or missing format, name or rights; no packages array; more than 5000 packages;
- bad package id (`a-z 0-9 . - _`), duplicate ids, missing version, **missing license**, missing kind;
- `sha256` not 64 lowercase hex; `size` not a whole number from 1 byte to 64 GiB;
- text longer than its limit or containing control characters;
- a download `url` that is not https, has user-info or a non-443 port, or is on **a different host than the index** (so a source cannot redirect downloads elsewhere).

Unknown fields are ignored and never acted on. Cached indexes are validated again on every read, so a tampered file on disk is not shown.

## Labels (derived by PNR, never taken from the index)

| Label | Meaning |
|---|---|
| supported | `kind` is `pnr-package`, the only format PNR can install |
| experimental | the same, and the source marks it experimental |
| unsupported | any other kind: listed for information, PNR cannot install it |

A label says what PNR can *handle*. It says nothing about whether a game runs: compatibility stays in the existing compatibility engine.

## Safety rules, each tested

- https only, TLS verification always on, no redirects followed (reuses the PSN HTTP layer).
- Each refresh may only talk to that one source's host.
- Remove deletes the source's cached index and never touches installed games.
- A refresh failure (transport, HTTP error, bad index) changes nothing.

## Downloads and install (slice A2)

Commands: `store get <src> <pkg>` (queue), `store queue list|run|pause|resume|cancel`
(`cancel` is a dry run unless `--yes`), `store install <src> <pkg>`.

- Streaming to `<store>/downloads/<src>/<pkg>.part` with a `.state` file. Resume uses `Range` with `If-Range`; a 206 must carry a matching `Content-Range`; a 200 or 416 restarts from zero.
- Bounded retries with backoff, a free-disk precheck, host limited to the index host.
- Size and sha256 are enforced before the final rename. A mismatch deletes the data. If the index hash changes, the old partial is discarded.
- Install re-hashes the file, accepts only `verified` or `installed` entries and only `pnr-package` kind, and uses the existing package importer. Other kinds are refused.
- Limits: hash only, no signatures, no scanning, no sandbox, Linux only. The libcurl streaming options (including low-speed abort) are hand-declared and were verified only against libcurl 7.81 and a local server.
- Tests: `tests/download_tests.cpp` (fake and local Range server) and the `pnr_store_workflow` CTest (tampered, foreign-kind and unverified refusals, good install).

## Signatures (slice B1)

- `store source pin <id> <64-hex Ed25519 public key>` pins a key for a source (`none` unpins). `store source list` shows the pin.
- A package in a pinned source needs a `signature` field (128 hex) in the index. It signs the text `pnr-store-v1\n<source id>\n<package id>\n<version>\n<sha256>\n<size>\n`, so a signature cannot be moved to another package, source, version, hash or size.
- `store install` refuses a pinned source's package that is unsigned or whose signature fails. A malformed `signature` field rejects the whole index.
- Unpinned sources stay hash-only and are labelled "unsigned (hash only)".
- The verifier is in-tree (`crypto/ed25519.*`, RFC 8032 pure Ed25519, SHA-512), tested with the RFC 8032 section 7.1 vectors, bit-flip, wrong-key and non-canonical-S cases. It is not constant-time, which is fine because it only handles public data. PNR holds no signing keys; `ed25519_sign` exists for tests and publisher tooling.
- What a signature means: the holder of the pinned key produced this package entry. It says nothing about whether the package is safe, and trust in the key is your decision.
- Launch-time re-check (B1b): `store install` writes `<store>/provenance/<game id>.prov` (source, package, version, sha256, size, signature, pinned key). `launch` re-verifies the signature and re-hashes the retained `.pnrgame` in `<store>/downloads/`. It refuses to start the game if the signature or retained hash fails, the source was removed, the pinned key changed, or the source now pins a key but the game was installed unsigned. Games not installed from the store are not affected. Unsigned installs from unpinned sources still run, labelled hash only.
- Limits: the launch check covers the retained package file and the signature, NOT the extracted install directory, and the provenance file is a local file, so someone who can already rewrite your store directory can edit it. There is no key rotation or revocation yet. Tested against fakes only.

## What is NOT done (honest list)

- **No update path yet.** Downloads and install exist (see below) but were only tested against fakes and a local server.
- **Signatures are opt-in per source (slice B1).** Without a pinned key, integrity means only the hash in the index. If the source is malicious or compromised, its hash proves nothing.
- No malware scanning and no sandbox (security slice, Planned). A verified hash does not mean a package is safe. Until then `pnr run` is for trusted developer-built demos only.
- Rights and licences are declared text. PNR does not verify them.
- No artwork, categories browsing UI, DLC, developer submission flow or Windows build/test yet. Linux only.

## Scanning (slice B3)

`store scan <source> <package>` and `store install` run the scan pipeline: hash (always), EMBER2024 model score through an external helper (OFF unless configured), ClamAV (OFF unless installed). A flagged stage refuses install unless `--allow-flagged`. A clean result never proves safety. Details, licences and the extractor decision: [docs/scan.md](scan.md).
