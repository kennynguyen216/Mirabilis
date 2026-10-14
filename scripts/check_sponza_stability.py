"""Check a bounded Sponza frame log: python scripts/check_sponza_stability.py <run.csv>."""
import csv
import math
from pathlib import Path
import statistics
import sys

path = Path(sys.argv[1])
rows = list(csv.DictReader(path.open(encoding="utf-8-sig")))
assert len(rows) == 300, "Movement run did not finish all 300 frames"
assert len({r["frame"] for r in rows}) == len(rows), "Repeated frame IDs"
assert all(0 < int(r["radiosity_texels"]) <= 128000 for r in rows), "Cache update exceeded its budget"
log = path.with_suffix(".log").read_text(encoding="utf-8")
assert "GI bounded run complete:" in log and "Detected Vulkan error:" not in log
for key in ("ms", "ssgi_gpu_ms", "radiosity_gpu_ms"):
    values = sorted(float(r[key]) for r in rows[5:])
    assert all(math.isfinite(v) and v >= 0 for v in values)
    print(f"{key}: median {statistics.median(values):.2f}, p99 {values[int(.99*(len(values)-1))]:.2f}, max {values[-1]:.2f}")
print("PASS: completed movement run, bounded cache work, no Vulkan error")
