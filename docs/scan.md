# PNR package scan pipeline (slice B3)

Status: **Pipeline Implemented + Tested with fakes. Model stage Experimental: it has NOT been run against real thrember or a real EMBER2024 model, and ClamAV has not been run for real either (neither exists in the development sandbox).**

Commands: `pnr-launcher store scan <source> <package>` (needs a downloaded, verified package), and `store install` runs the same scan first. `--allow-flagged` overrides a flagged install.

## Stages

| Stage | When it runs | States |
|---|---|---|
| hash | always | ok, flagged (size or sha256 differs from the index), error |
| model | only if BOTH `PNR_SCAN_MODEL` (an EMBER2024 ELF `.model`) and `PNR_SCAN_HELPER` (the helper script) are set and exist, and the interpreter is found | off, ok, flagged, error, not-applicable |
| clamav | only if `clamscan` is on PATH (or `PNR_SCAN_CLAMSCAN`) | off, ok, flagged, error |

- **"off" is reported with its reason, never faked.** Nothing is scored when a stage is off.
- A **flagged** stage refuses `store install` unless `--allow-flagged`. An **error** or **off** stage never blocks and never counts as a pass. A clean result never proves safety: **detection is high but not absolute.**
- The model stage scores only the **ELF files inside** a `.pnrgame`. The package container itself is never scored, and a package without ELF files reports `not-applicable`.
- The score threshold is `PNR_SCAN_THRESHOLD` (default 0.5). **PNR has not calibrated it.** The highest score over the ELF files is reported.
- This is one layer, not the only gate: the order is hash, signature (if the source pins a key), scan, then the optional sandbox at launch.

## Real captured output (no scanner configured, this build host)

```
$ pnr-launcher store scan demo good
SCAN
  hash: ok - size and sha256 match the index
  model: off - not configured (needs PNR_SCAN_MODEL and PNR_SCAN_HELPER); no model scoring happened
  clamav: off - clamscan not found: no ClamAV scan happened
  note: detection is high but not absolute. A clean result is NOT a guarantee the package is safe.
```

## Licence check (read from the sources on 2026-11-14)

- EMBER2024 / thrember repository: **Apache-2.0** (LICENSE file and repo page, https://github.com/FutureComputing4AI/EMBER2024).
- EMBER2024 models: Hugging Face repo `joyce8/EMBER2024-benchmark-models`, model card says **License: apache-2.0**; 14 LightGBM models (including `EMBER2024_ELF.model`), about 512 MB in total (https://huggingface.co/joyce8/EMBER2024-benchmark-models).
- thrember dependencies (from its `pyproject.toml`): lightgbm, pefile, polars, scikit-learn, tqdm, signify, huggingface_hub, matplotlib. I did not check each of these licences, so PNR vendors none of them.
- **Nothing is bundled with PNR.** Users install thrember and download the model themselves (`thrember.download_models(dir)`), so no redistribution happens through this repo.

## Extractor decision: external Python helper, not a C++ port

Evidence:
1. thrember's `predict_sample(model, bytes)` builds a `PEFeatureExtractor` vector and calls `Booster.predict` (src/thrember/model.py). The extractor is the full EMBER feature version 3 set. Non-PE files use a subset (general file info, byte histogram, byte-entropy histogram and string statistics), and the extractor takes an optional `features_file` to pick which blocks a model uses. **I could not establish from the sources which blocks the published ELF model expects**, so any port would be guessing.
2. The string statistics use about 76 Python `re` patterns over bytes, and the other blocks use numpy float32 math. A C++ port would have to reproduce them exactly, and I have no thrember, numpy or lightgbm in the sandbox to produce reference vectors to test against.
3. So a port cannot be verified here. The helper (`tools/pnr_scan_helper.py`) calls the official code, and refuses with exit 3 if the model's feature count differs from the extractor's, rather than guessing.

A C++ port stays a Planned option only if someone can produce reference vectors from real thrember and test against them.

## Limits

- The helper runs as your user without the sandbox (Planned: run it under the B2 sandbox). It parses untrusted files with pefile, so keep it updated.
- ClamAV signatures may be out of date. EMBER2024 reports detection quality in its paper; PNR makes no accuracy claim.
- Not run against real software in development: only fake helpers and a fake `clamscan` script.
