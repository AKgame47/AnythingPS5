#!/usr/bin/env python3
"""PNR scan helper (slice B3, EXPERIMENTAL).

Scores one file with an EMBER2024 LightGBM model through the official thrember
feature extractor, then prints one JSON line: {"score": <0..1>}.
Usage: pnr_scan_helper.py --model EMBER2024_ELF.model --file sample.bin

Needs: pip install thrember lightgbm (thrember pulls in pefile, polars, etc.).
Models: thrember.download_models("<dir>") from the joyce8/EMBER2024-benchmark-models
Hugging Face repo (Apache-2.0 per its model card). Nothing is bundled with PNR.

This follows thrember.predict_sample (extractor.feature_vector, then
Booster.predict). It was NOT run against real thrember or a real model in the
PNR development sandbox. If the model's feature count differs from the
extractor's, it refuses instead of guessing which features the model used.
A score is not a verdict: a low score is not a guarantee of safety.
"""
import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--file", required=True)
    a = ap.parse_args()
    try:
        import lightgbm as lgb
        import numpy as np
        import thrember
    except Exception as e:  # missing dependency: say so, do not fake a score
        print("missing dependency (thrember, lightgbm, numpy): %s" % e, file=sys.stderr)
        return 2
    with open(a.file, "rb") as f:
        data = f.read()
    booster = lgb.Booster(model_file=a.model)
    vec = np.array(thrember.PEFeatureExtractor().feature_vector(data), dtype=np.float32)
    if booster.num_feature() != vec.shape[0]:
        print("feature count mismatch: model expects %d, extractor gives %d; refusing to guess"
              % (booster.num_feature(), vec.shape[0]), file=sys.stderr)
        return 3
    print(json.dumps({"score": float(booster.predict([vec])[0])}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
