"""Exercise production managed startup in project-only fixtures, without CUDA."""
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
fixture = ROOT / "build/安装 参数 regression"
state = ROOT / "build/安装 状态 regression"
runtime = ROOT / "dist/RvcFeatureInjector-1.1.0-portable/rvc-runtime"
engine = ROOT / "build/Release/rvc_engine_regression.exe"
marker_text = b"TAKETO-RVCFEATUREINJECTOR-STATE-V1\n"
(fixture / "tools").mkdir(parents=True, exist_ok=True)
(fixture / "runtime-root.txt").write_text(str(runtime) + "\n", encoding="utf-8-sig")
# A final backslash exercises Windows command-line quoting and Unicode/spaces.
expected_state = str(state) + "\\"
(fixture / "state-root.txt").write_text(expected_state + "\r\n", encoding="utf-8-sig")
original = (ROOT / "tests/fixtures/rvc_engine_worker.py").read_text(encoding="utf-8")
probe = '''import json, sys
from pathlib import Path
import argparse
parser = argparse.ArgumentParser()
parser.add_argument("--runtime-root", required=True)
parser.add_argument("--state-root", required=True)
args = parser.parse_args()
assert len(sys.argv) == 5, sys.argv
assert sys.dont_write_bytecode
assert args.state_root == EXPECTED_STATE, sys.argv
assert args.runtime_root == EXPECTED_RUNTIME, sys.argv
assert (Path(args.state_root) / ".taketo-rvc-state").read_bytes() == EXPECTED_MARKER
with (Path(args.state_root) / "startup.json").open("w", encoding="utf-8") as output:
    json.dump({"argv": sys.argv, "dont_write_bytecode": sys.dont_write_bytecode}, output, ensure_ascii=False)
print("managed startup diagnostic", file=sys.stderr)
'''.replace("EXPECTED_STATE", repr(expected_state)).replace("EXPECTED_RUNTIME", repr(str(runtime))).replace(
    "EXPECTED_MARKER", repr(marker_text))
(fixture / "tools/rvc_daemon_worker.py").write_text(probe + original, encoding="utf-8")
env = dict(os.environ)
env.pop("RVC_RUNTIME_ROOT", None)


def run(mode="transaction", check=True):
    return subprocess.run([str(engine), str(fixture), mode], cwd=ROOT, env=env,
                          check=check, timeout=40, capture_output=not check)


# The helper invokes the real Windows lookup read-only and separately checks token
# expansion using this fixture path; no process writes into the actual profile.
(fixture / "state-root.txt").write_text("%LOCALAPPDATA%\\Taketo\\RvcFeatureInjector\\state\n", encoding="utf-8")
run("state-paths")
(fixture / "state-root.txt").write_text(expected_state + "\r\n", encoding="utf-8-sig")
if state.exists():
    assert state.resolve().is_relative_to((ROOT / "build").resolve())
    shutil.rmtree(state)
state.mkdir(parents=True)
(state / "old-cache.txt").write_text("previous managed state", encoding="utf-8")
run()
assert (state / ".taketo-rvc-state").read_bytes() == marker_text
assert (state / "old-cache.txt").is_file()
result = json.loads((state / "startup.json").read_text(encoding="utf-8"))
assert result["argv"][-1] == expected_state
logs = list((state / "logs").glob("*.log"))
assert logs
assert any("managed startup diagnostic" in log.read_text(encoding="utf-8") for log in logs)
assert not list((fixture / "tools").glob("__pycache__"))

# Never claim another owner's state by replacing a marker.
(state / ".taketo-rvc-state").write_bytes(b"unknown owner\n")
(state / "startup.json").unlink()
assert run(check=False).returncode != 0
assert (state / ".taketo-rvc-state").read_bytes() == b"unknown owner\n"
assert not (state / "startup.json").exists()

# Even a failed daemon initialization leaves a valid cleanup marker.
(state / ".taketo-rvc-state").unlink()
(fixture / "tools/rvc_daemon_worker.py").write_text("raise RuntimeError('fixture fails before init')\n", encoding="utf-8")
assert run(check=False).returncode != 0
assert (state / ".taketo-rvc-state").read_bytes() == marker_text
assert not (state / "startup.json").exists()
print(json.dumps({"passed": True, "checks": ["token resolution", "absolute state path", "marker before worker",
    "legacy managed state", "foreign marker rejected", "failed startup cleanup marker"], "state": str(state)}, ensure_ascii=False))
