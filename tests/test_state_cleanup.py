"""Native cleanup safety checks; every fixture stays inside this project."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/Release/rvc-state-cleanup.exe"
ART = ROOT / "artifacts/installer-1.1.6-2026-11-11"
MARKER = b"TAKETO-RVCFEATUREINJECTOR-STATE-V1\n"
RESULTS = []
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]


def write(path, contents=b"cache fixture"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)


def state(base, name):
    path = base / name / "Taketo/RvcFeatureInjector/state"
    write(path / ".taketo-rvc-state", MARKER)
    write(path / "cache/深层 目录/generated.bin")
    write(path / "logs/worker.log")
    return path


def invoke(path, mode="--check", expected=0):
    result = subprocess.run([str(EXE), mode, str(path)], capture_output=True, timeout=20)
    if expected == "deny":
        assert result.returncode not in (0, 2), (path, result.returncode, result.stderr)
    else:
        assert result.returncode == expected, (path, result.returncode, result.stderr)
    RESULTS.append({"case": f"{path.parent.parent.parent.name}:{mode}", "exit_code": result.returncode})


def junction(link, target):
    link.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)


def main():
    ART.mkdir(parents=True, exist_ok=True)
    base = Path(tempfile.mkdtemp(prefix="native-cleanup-中文 ", dir=ART))
    assert base.resolve().is_relative_to(ART.resolve())
    links = []
    try:
        owned = state(base, "normal")
        sentinel = owned.parent / "user-model.pth"
        write(sentinel, b"preserve sibling")
        snapshot = {str(p.relative_to(owned)): p.read_bytes() for p in owned.rglob("*") if p.is_file()}
        invoke(owned)
        assert snapshot == {str(p.relative_to(owned)): p.read_bytes() for p in owned.rglob("*") if p.is_file()}
        invoke(owned, "--remove")
        assert not owned.exists() and sentinel.read_bytes() == b"preserve sibling"
        invoke(owned, "--remove", expected=2)

        unmarked = state(base, "unmarked")
        (unmarked / ".taketo-rvc-state").unlink()
        invoke(unmarked, "--remove", expected=2)
        assert (unmarked / "logs/worker.log").exists()

        foreign = state(base, "foreign-marker")
        write(foreign / ".taketo-rvc-state", b"foreign owner\n")
        invoke(foreign, "--remove", expected=2)
        assert (foreign / ".taketo-rvc-state").read_bytes() == b"foreign owner\n"

        wrong = base / "invalid-suffix/state"
        write(wrong / ".taketo-rvc-state", MARKER)
        invoke(wrong, "--remove", expected="deny")
        assert (wrong / ".taketo-rvc-state").exists()

        locked = state(base, "locked-file")
        handle = kernel.CreateFileW(str(locked / "logs/worker.log"), 0x80000000, 1, None, 3, 0, None)
        assert handle not in (None, ctypes.c_void_p(-1).value), ctypes.get_last_error()
        try:
            invoke(locked, expected="deny")
            invoke(locked, "--remove", expected="deny")
            assert (locked / ".taketo-rvc-state").read_bytes() == MARKER
            assert (locked / "cache/深层 目录/generated.bin").exists()
        finally:
            kernel.CloseHandle(handle)
        invoke(locked, "--remove")

        writable = state(base, "writable-directory-handle")
        handle = kernel.CreateFileW(str(writable), 0x40000000, 7, None, 3, 0x02000000, None)
        assert handle not in (None, ctypes.c_void_p(-1).value), ctypes.get_last_error()
        try:
            invoke(writable, expected="deny")
            invoke(writable, "--remove", expected="deny")
            assert (writable / "logs/worker.log").exists()
        finally:
            kernel.CloseHandle(handle)

        outside = base / "unrelated-preserve"
        write(outside / "sentinel.bin", b"must not delete")
        descendant = state(base, "child-junction")
        link = descendant / "cache/redirect"
        junction(link, outside)
        links.append(link)
        invoke(descendant, expected="deny")
        invoke(descendant, "--remove", expected="deny")
        assert (outside / "sentinel.bin").read_bytes() == b"must not delete"
        link.rmdir()
        links.remove(link)

        actual = state(base, "root-junction-target")
        link = base / "root-junction/Taketo/RvcFeatureInjector/state"
        junction(link, actual)
        links.append(link)
        invoke(link, expected="deny")
        invoke(link, "--remove", expected="deny")
        assert (actual / ".taketo-rvc-state").read_bytes() == MARKER
        link.rmdir()
        links.remove(link)

        real_ancestor = base / "ancestor-target"
        nested = state(real_ancestor, "profile")
        link = base / "ancestor-junction"
        junction(link, real_ancestor / "profile")
        links.append(link)
        invoke(link / "Taketo/RvcFeatureInjector/state", expected="deny")
        invoke(link / "Taketo/RvcFeatureInjector/state", "--remove", expected="deny")
        assert (nested / "logs/worker.log").exists()
        link.rmdir()
        links.remove(link)

        marker_link = state(base, "marker-junction")
        (marker_link / ".taketo-rvc-state").unlink()
        link = marker_link / ".taketo-rvc-state"
        junction(link, outside)
        links.append(link)
        invoke(marker_link, "--remove", expected="deny")
        assert (outside / "sentinel.bin").read_bytes() == b"must not delete"
        link.rmdir()
        links.remove(link)
        print(json.dumps({"passed": True, "checks": len(RESULTS), "results": RESULTS}, ensure_ascii=False))
    finally:
        for link in links:
            if link.is_junction():
                link.rmdir()
        assert base.resolve().is_relative_to(ART.resolve())
        shutil.rmtree(base)


if __name__ == "__main__":
    main()
