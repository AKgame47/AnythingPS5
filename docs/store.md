# PNR Unofficial Store: core (slice A1)

Status: **Implemented + Tested (local fakes only).** Sources, index
validation, refresh and search work. **There is no download, install or
update yet** (next slice). Nothing here has been run against a real source.

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

## What is NOT done (honest list)

- **No download manager, install or update path yet.** `sha256` and `size` are validated and shown, but nothing is fetched or checked against them until the download slice.
- **No signatures.** Integrity today means the hash in the index. If the source is malicious or compromised, its hash proves nothing. Per-source pinned Ed25519 keys are Planned.
- No malware scanning and no sandbox (security slice, Planned). Until then `pnr run` is for trusted developer-built demos only.
- Rights and licences are declared text. PNR does not verify them.
- No artwork, categories browsing UI, DLC, developer submission flow or Windows build/test yet. Linux only.
