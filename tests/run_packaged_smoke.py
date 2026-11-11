"""Load the relocated release DLL and execute real RVC inference through its configured runtime."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--bundle", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--model-a", type=Path, required=True)
parser.add_argument("--model-b", type=Path, required=True)
parser.add_argument("--index-a", type=Path)
parser.add_argument("--index-b", type=Path)
args = parser.parse_args()
for slot in ("A", "B"):
    model = getattr(args, f"model_{slot.lower()}").resolve()
    index = getattr(args, f"index_{slot.lower()}")
    assert model.is_file(), model
    os.environ[f"RVC_TEST_MODEL_{slot}"] = str(model)
    os.environ[f"RVC_TEST_INDEX_{slot}"] = str(index.resolve()) if index else ""
args.output.parent.mkdir(parents=True, exist_ok=True)
cache = args.output.parent / "packaged-numba-cache"
cache.mkdir(exist_ok=True)
os.environ.setdefault("NUMBA_CACHE_DIR", str(cache.resolve()))
os.environ.pop("RVC_RUNTIME_ROOT", None)
bundle = args.bundle.resolve()
library_path = bundle / "Contents/x86_64-win/RvcFeatureInjector.vst3"
config_path = bundle / "Contents/Resources/runtime-root.txt"
runtime_root = (config_path.parent / config_path.read_text(encoding="utf-8-sig").strip()).resolve()
assert (runtime_root / "runtime/python.exe").is_file(), runtime_root
library = ctypes.WinDLL(str(library_path))
verify = library.RvcVst3_VerifyNeuralCacheE2E
verify.argtypes = [ctypes.POINTER(ctypes.c_float)] * 4
verify.restype = ctypes.c_bool
coverage, rms, correlation, seconds = (ctypes.c_float() for _ in range(4))
success = verify(*(ctypes.byref(value) for value in (coverage, rms, correlation, seconds)))
result = {"success": success, "coverage": coverage.value, "wet_rms": rms.value,
          "dry_correlation": correlation.value, "synth_seconds": seconds.value,
          "runtime_root": str(runtime_root), "bundle": str(bundle),
          "binary_sha256": hashlib.sha256(library_path.read_bytes()).hexdigest()}
assert success and coverage.value >= .999 and rms.value > .005 and abs(correlation.value) < .97, result
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(result, ensure_ascii=False), flush=True)
