"""Build with an existing project-local or installed CMake/MSVC toolchain."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", action="store_true")
    args = parser.parse_args()
    candidates = [
        ROOT / ".tools/cmake/bin/cmake.exe",
        Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    ]
    cmake = shutil.which("cmake") or next((str(p) for p in candidates if p.is_file()), None)
    if not cmake:
        raise SystemExit("CMake is unavailable. Place a portable CMake in .tools/cmake.")
    env = {key.upper(): value for key, value in os.environ.items()}
    cache = ROOT / "build/runtime-cache"
    cache.mkdir(parents=True, exist_ok=True)
    env["NUMBA_CACHE_DIR"] = str(cache)
    subprocess.run([cmake, "-S", str(ROOT), "-B", str(ROOT / "build"), "-G", "Visual Studio 17 2022", "-A", "x64"], env=env, check=True)
    subprocess.run([cmake, "--build", str(ROOT / "build"), "--config", "Release", "--parallel", "4"], env=env, check=True)
    version = re.search(r'^#define RVC_PLUGIN_VERSION "([^"]+)"',
                        (ROOT / "src/PluginVersion.h").read_text(encoding="utf-8"), re.MULTILINE).group(1)
    metadata_path = ROOT / "dist/RvcFeatureInjector.vst3/Contents/Resources/moduleinfo.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["Version"] = version
    for entry in metadata["Classes"]:
        entry["Version"] = version
    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.test:
        subprocess.run([str(ROOT / "build/Release/dsp_regressions.exe")], cwd=ROOT, env=env, check=True)
        subprocess.run([str(ROOT / "build/Release/dsp_seam_regressions.exe")], cwd=ROOT, env=env, check=True)
        subprocess.run([str(ROOT / "build/Release/dsp_cache_regressions.exe")], cwd=ROOT, env=env, check=True)
        subprocess.run([str(ROOT / "build/Release/dsp_capture_startup_regressions.exe")], cwd=ROOT, env=env, check=True)
        subprocess.run([sys.executable, str(ROOT / "tests/run_engine_regressions.py")], cwd=ROOT, env=env, check=True)
        subprocess.run([str(ROOT / "build/Release/vst3_host_verify.exe")], cwd=ROOT, env=env, check=True)
        subprocess.run([sys.executable, str(ROOT / "tests/verify_capture_editor.py")], cwd=ROOT, env=env, check=True)
        subprocess.run([sys.executable, str(ROOT / "tests/verify_managed_startup.py")], cwd=ROOT, env=env, check=True)
        subprocess.run([sys.executable, str(ROOT / "tests/test_state_cleanup.py")], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
