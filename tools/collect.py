#!/usr/bin/env python3
"""collect.py — rebuild data/v2/<name>.csv from the per-run files in data/v2/runs/<name>/.
Safe to run at any time (also while experiments are running): it only reads finished files."""
import glob, os, sys
import pandas as pd
out = sys.argv[1] if len(sys.argv) > 1 else "data/v2"
runs = os.path.join(out, "runs")
for d in sorted(glob.glob(os.path.join(runs, "*"))):
    files = sorted(f for f in glob.glob(os.path.join(d, "*.csv")) if not os.path.basename(f).startswith(".tmp_"))
    if not files:
        continue
    df = pd.concat([pd.read_csv(f) for f in files], ignore_index=True)
    target = os.path.join(out, os.path.basename(d) + ".csv")
    tmp = target + ".tmp"
    df.to_csv(tmp, index=False)
    os.replace(tmp, target)
    print(f"{os.path.basename(target):28s} {len(df):6d} rows")
