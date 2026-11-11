"""Render real RVC append/prepend conversion at 44.1 kHz / 128 samples."""
import ctypes
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/append-cache-1.1.8/transport-neural"
OUT.mkdir(parents=True, exist_ok=True)
assert all(os.environ.get("RVC_TEST_MODEL_"+slot) for slot in ("A", "B")), "Provide explicit test models"
library = ctypes.WinDLL(str(ROOT / "dist/RvcFeatureInjector.vst3/Contents/x86_64-win/RvcFeatureInjector.vst3"))
verify = library.RvcVst3_VerifyAppendAudio
verify.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_float)]
verify.restype = ctypes.c_bool
metrics = (ctypes.c_float*5)()
passed = verify(str(OUT), metrics)
result = {"passed": passed, "sample_rate": 44100, "buffer_samples": 128,
          "synthesis_seconds": metrics[0], "coverage": metrics[1],
          "minimum_5ms_boundary_rms_ratio": metrics[2], "longest_zero_run_ms": metrics[3],
          "dry_wet_correlation": metrics[4],
          "boundaries_seconds": [2, 7, 12], "explicit_external_test_models": True,
          "mid_note_stop_return_modes": ["stopped return", "delayed playing return", "frozen playing position"],
          "minimum_20ms_repeat_rms_ratio_required": .98}
(OUT / "results.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
print(json.dumps(result), flush=True)
assert passed, "Append neural continuity regression; inspect generated audio"
