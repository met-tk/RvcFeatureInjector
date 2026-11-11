"""Asset-backed high-note regression against the production worker's real HuBERT/RMVPE."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--runtime-root", default=os.environ.get("RVC_RUNTIME_ROOT", r"D:\RVC20260718Nvidia"))
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "artifacts" / "rvc-fix-verification" / "neural-high-note.json")
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
cache_dir = args.output.parent / "numba-cache"
cache_dir.mkdir(exist_ok=True)
os.environ["NUMBA_CACHE_DIR"] = str(cache_dir)
os.environ["RVC_CUDA_GRAPH"] = "0"
os.environ["RVC_RUNTIME_ROOT"] = args.runtime_root
sys.argv.append("--test-cli")
worker_path = Path(__file__).resolve().parents[1] / "tools" / "rvc_daemon_worker.py"
spec = importlib.util.spec_from_file_location("rvc_neural_worker", worker_path)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)
np = worker.np
worker.torch.set_num_threads(4)
engine = worker.RvcDaemonEngine()
engine.ensure_base_models()

f0 = 740.0
sr = 16000
t = np.arange(sr, dtype=np.float64) / sr
x = np.zeros_like(t)
for harmonic in range(1, int(7500 / f0) + 1):
    freq = harmonic * f0
    envelope = np.exp(-0.5 * ((freq - 900) / 200) ** 2)
    envelope += 2.8 * np.exp(-0.5 * ((freq - 2800) / 250) ** 2)
    envelope += 1.6 * np.exp(-0.5 * ((freq - 3600) / 300) ** 2)
    x += envelope / harmonic ** 0.6 * np.sin(2 * np.pi * freq * t)
x = (x / np.max(np.abs(x)) * 0.3).astype(np.float32)
filtered = worker.signal.filtfilt(worker.HIGHPASS_BH, worker.HIGHPASS_AH, x).astype(np.float32)
padded = np.pad(filtered, (3200, 3360), mode="reflect").astype(np.float32)
raw_f0 = engine.rmvpe.infer_from_audio(padded, thred=0.045)
entry, _ = engine._extract_or_get_cached_features(padded, 0, 1)
actual_f0 = entry[2]
interior = slice(20, 120)
voiced = actual_f0[interior][actual_f0[interior] > 0]
result = {
    "runtime_root": worker.RVC_RUNTIME_ROOT,
    "device": str(engine.device),
    "half_precision": engine.is_half,
    "input_f0": f0,
    "interior_frames": 100,
    "raw_voiced_frames": int((raw_f0[interior] > 0).sum()),
    "worker_voiced_frames": len(voiced),
    "worker_median_f0": float(np.median(voiced)) if len(voiced) else 0,
    "feature_shapes": [list(entry[0].shape), list(entry[1].shape)],
}
np.testing.assert_array_equal(actual_f0, raw_f0.astype(np.float32))
assert len(voiced) >= 95, result
assert abs(result["worker_median_f0"] - f0) < 25, result
result["passed"] = True
output = json.dumps(result, ensure_ascii=False, indent=2)
args.output.write_text(output + "\n", encoding="utf-8")
print(output, flush=True)
