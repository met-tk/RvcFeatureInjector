"""Run production IPC regressions against a small protocol fixture, without CUDA."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
fixture = ROOT / "build/engine-regression-fixture"
(fixture / "tools").mkdir(parents=True, exist_ok=True)
shutil.copy2(ROOT / "tests/fixtures/rvc_engine_worker.py", fixture / "tools/rvc_daemon_worker.py")
env = {key.upper(): value for key, value in os.environ.items()}
for mode in ("transaction", "init", "load", "synth-read", "synth-write", "timeout", "reply-length"):
    subprocess.run([str(ROOT / "build/Release/rvc_engine_regression.exe"), str(fixture), mode],
                   cwd=ROOT, env=env, timeout=40, check=True)
