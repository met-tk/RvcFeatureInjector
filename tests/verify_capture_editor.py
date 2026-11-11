"""Verify new/append Capture through the real editor's Windows mouse handling."""
import ctypes
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/installer-1.1.8-2026-11-11/capture-editor"
OUT.mkdir(parents=True, exist_ok=True)
library = ctypes.WinDLL(str(ROOT / "dist/RvcFeatureInjector.vst3/Contents/x86_64-win/RvcFeatureInjector.vst3"))
verify = library.RvcVst3_VerifyAppendCaptureUi
verify.argtypes = [ctypes.c_wchar_p]
verify.restype = ctypes.c_bool
success = verify(str(OUT))
result = {"passed": success, "checks": ["logo version details", "About Esc", "empty append disabled", "new capture", "stop capture",
          "append armed", "DAW stop ends append", "rearm prepend and append", "stop append", "generate merged capture",
          "new capture replaces previous"], "snapshots": sorted(p.name for p in OUT.glob("*.png"))}
(OUT / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
assert success, "Capture editor interaction regression; see generated snapshots"
