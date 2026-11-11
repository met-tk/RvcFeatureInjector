"""Run the actual Inno install/upgrade/uninstall in a project-only sandbox, no registry entry."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "artifacts/installer-1.1.9-2026-11-11"
BASE = ART / "fixture-tests"
PAYLOAD = BASE / "payload"
SANDBOX = BASE / "中文 沙盒"
APP = SANDBOX / "app/RvcFeatureInjector"
PLUGIN = SANDBOX / "VST3/RvcFeatureInjector.vst3"
RESULTS = []


def write(path, text="fixture"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def remove(path):
    assert path.resolve().is_relative_to(BASE.resolve()) and path != BASE
    if path.exists():
        shutil.rmtree(path)


def run(exe, name, *extra, success=True):
    log = BASE / f"{name}.log"
    log.unlink(missing_ok=True)
    result = subprocess.run([str(exe), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                             "/SP-", f"/LOG={log}", *extra], timeout=90)
    # Uninstaller self-copies; wait for its actual log completion, not just its launcher.
    for _ in range(120):
        if log.exists() and "Log closed." in log.read_text(encoding="utf-8-sig"):
            break
        time.sleep(0.1)
    text = log.read_text(encoding="utf-8-sig")
    if success:
        assert result.returncode == 0, (name, result.returncode, text[-3000:])
    else:
        assert result.returncode != 0 or "Aborted by InitializeUninstall" in text, (name, text[-3000:])
    RESULTS.append({"case": name, "exit_code": result.returncode, "passed": True})
    return text


def compile_version(version, helper=None):
    helper_args = ["--cleanup-helper", str(helper)] if helper else []
    with (BASE / f"compile-{version}.log").open("w", encoding="utf-8") as log:
        subprocess.run([sys.executable, str(ROOT / "tools/build_installer.py"),
                        "--payload-root", str(PAYLOAD), "--plugin-root", str(PAYLOAD / PLUGIN.name),
                        "--test-root", str(SANDBOX), "--version", version,
                        "--output-dir", str(BASE / "setups"), *helper_args], check=True, stdout=log, stderr=log)
    return BASE / f"setups/RvcFeatureInjector-Test-{version}.exe"


def uninstall(name="uninstall", **kwargs):
    return run(APP / "unins000.exe", name, **kwargs)


def assert_clean():
    assert not APP.exists(), list(APP.rglob("*")) if APP.exists() else ""
    assert not PLUGIN.exists(), list(PLUGIN.rglob("*")) if PLUGIN.exists() else ""


def main():
    RESULTS.clear()
    BASE.mkdir(parents=True, exist_ok=True)
    assert not APP.exists() and not PLUGIN.exists(), "Remove prior test install with its uninstaller first"
    remove(PAYLOAD)
    remove(SANDBOX / "profiles")
    write(PAYLOAD / "rvc-runtime/runtime/python.exe")
    write(PAYLOAD / "rvc-runtime/runtime/obsolete.txt")
    write(PAYLOAD / "rvc-runtime/assets/base.bin")
    write(PAYLOAD / PLUGIN.name / "Contents/x86_64-win/RvcFeatureInjector.vst3")
    write(PAYLOAD / PLUGIN.name / "Contents/Resources/tools/rvc_daemon_worker.py")
    write(PAYLOAD / PLUGIN.name / "Contents/Resources/models/preset.pth")
    write(PAYLOAD / "rvc-runtime/runtime/__pycache__/skip.pyc")
    write(PAYLOAD / PLUGIN.name / "Contents/Resources/models/__pycache__/skip.pyc")
    first = compile_version("0.0.1")

    write(APP / "user.txt", "unmanaged app")
    run(first, "reject-unmanaged-app", success=False)
    assert (APP / "user.txt").read_text() == "unmanaged app"
    remove(APP)
    write(PLUGIN / "user.txt", "unmanaged plugin")
    run(first, "reject-unmanaged-plugin", success=False)
    assert (PLUGIN / "user.txt").read_text() == "unmanaged plugin"
    remove(PLUGIN)

    run(first, "rollback-injected-file-error", "/FixtureAbort=1", success=False)
    assert_clean()
    run(first, "install-chinese-spaces")
    resource = PLUGIN / "Contents/Resources"
    assert (resource / "runtime-root.txt").read_text(encoding="utf-8").strip() == str(APP / "rvc-runtime")
    assert (resource / "state-root.txt").read_text(encoding="utf-8").strip() == str(APP / "state")
    assert (APP / "plugin-root.txt").read_text(encoding="utf-8").strip() == str(PLUGIN)
    assert not (resource / "models/preset.pth").exists()
    manifest = (APP / "install-files.txt").read_text(encoding="utf-8-sig")
    assert "Contents/Resources/models/" not in manifest
    assert not list(SANDBOX.rglob("*.pyc"))

    # A held file simulates a host using the bundle, without launching/killing Cubase.
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    dll = PLUGIN / "Contents/x86_64-win/RvcFeatureInjector.vst3"
    handle = kernel.CreateFileW(str(dll), 0x80000000, 1, None, 3, 0, None)
    assert handle != wintypes.HANDLE(-1).value
    try:
        run(first, "reject-install-locked-file", success=False)
        uninstall("reject-uninstall-locked-file", success=False)
        assert dll.exists() and (APP / "unins000.exe").exists()
    finally:
        kernel.CloseHandle(handle)

    # Link creation stays within the test area and is removed without following it.
    outside = BASE / "outside"
    write(outside / "keep.txt", "outside sentinel")
    link = APP / "state/linked"
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)], check=True, capture_output=True)
    try:
        run(first, "reject-install-junction", success=False)
        uninstall("reject-uninstall-junction", success=False)
        assert (outside / "keep.txt").read_text() == "outside sentinel"
    finally:
        os.rmdir(link)

    # Upgrade removes its obsolete file, never a path escaping the owned roots.
    with (APP / "install-files.txt").open("a", encoding="utf-8") as manifest:
        manifest.write("app|../../outside/keep.txt\n")
    write(APP / "user-model.pth", "user app sentinel")
    write(resource / "models/custom.pth", "user plugin sentinel")
    write(APP / "state/cache/generated.bin", "disposable cache")
    (PAYLOAD / "rvc-runtime/runtime/obsolete.txt").unlink()
    write(PAYLOAD / "rvc-runtime/runtime/new.txt", "new version")
    second = compile_version("0.0.2")
    old_manifest = (APP / "install-files.txt").read_bytes()
    run(second, "failed-upgrade-restore-inventory", "/FixtureAbort=1", success=False)
    assert (APP / "install-files.txt").read_bytes() == old_manifest
    assert not (APP / "rvc-runtime/runtime/new.txt").exists()
    assert (APP / "unins000.exe").exists()
    run(second, "upgrade")
    assert not (APP / "rvc-runtime/runtime/obsolete.txt").exists()
    assert (APP / "rvc-runtime/runtime/new.txt").exists()
    assert (outside / "keep.txt").read_text() == "outside sentinel"
    uninstall("uninstall-preserve-user-files")
    remaining = {p.relative_to(SANDBOX).as_posix() for p in SANDBOX.rglob("*") if p.is_file()}
    assert remaining == {"app/RvcFeatureInjector/user-model.pth",
                         "VST3/RvcFeatureInjector.vst3/Contents/Resources/models/custom.pth"}, remaining
    remove(APP)
    remove(PLUGIN)

    run(second, "reinstall")
    write(APP / "state/logs/daemon.log")
    uninstall("uninstall-completely-clean")
    assert_clean()

    # Each destination is independently selected and persisted for the uninstaller.
    custom_parent = SANDBOX / "自选 VST3 plugins"
    custom_bundle = custom_parent / PLUGIN.name
    run(second, "install-custom-vst3-directory", f"/VST3DIR={custom_parent}")
    assert not PLUGIN.exists()
    assert (custom_bundle / "Contents/x86_64-win/RvcFeatureInjector.vst3").exists()
    assert (APP / "plugin-root.txt").read_text(encoding="utf-8").strip() == str(custom_bundle)
    moved_parent = SANDBOX / "other VST3"
    run(second, "reject-upgrade-vst3-relocation", f"/VST3DIR={moved_parent}", success=False)
    assert not moved_parent.exists()
    # Without a flag, upgrade must find the previous custom destination itself.
    run(second, "upgrade-reuse-custom-vst3-directory")
    assert not PLUGIN.exists() and custom_bundle.exists()
    # Corrupting one copy must not redirect uninstall to an unrelated location.
    stored = (APP / "plugin-root.txt").read_bytes()
    write(APP / "plugin-root.txt", str(outside))
    uninstall("reject-mismatched-plugin-root", success=False)
    assert custom_bundle.exists() and (outside / "keep.txt").exists()
    (APP / "plugin-root.txt").write_bytes(stored)
    uninstall("uninstall-custom-vst3-directory")
    assert not APP.exists() and not custom_bundle.exists() and not PLUGIN.exists()

    run(second, "reject-relative-vst3-directory", "/VST3DIR=relative-folder", success=False)
    assert not APP.exists() and not PLUGIN.exists()
    run(second, "reject-nested-vst3-bundle", f"/VST3DIR={SANDBOX / 'Nested.vst3'}", success=False)
    run(second, "reject-overlapping-vst3-directory", f"/VST3DIR={APP / 'plugins'}", success=False)
    assert not APP.exists() and not PLUGIN.exists()

    # Existing1.1.1 markers carry the bundle path in line3, before plugin-root existed.
    run(second, "install-for-legacy-path-fallback", f"/VST3DIR={custom_parent}")
    (APP / "plugin-root.txt").unlink()
    run(second, "upgrade-legacy-path-fallback")
    assert (APP / "plugin-root.txt").read_text(encoding="utf-8").strip() == str(custom_bundle)
    (APP / "plugin-root.txt").unlink()
    uninstall("uninstall-legacy-path-fallback")
    assert not APP.exists() and not custom_bundle.exists() and not PLUGIN.exists()

    # An old tracked bundled preset is pruned on upgrade; untracked user imports stay.
    run(second, "install-for-removing-old-bundled-model")
    retired = PLUGIN / "Contents/Resources/models/old-bundled.pth"
    imported = PLUGIN / "Contents/Resources/models/own-import.pth"
    write(retired, "old packaged model")
    write(imported, "user import")
    with (APP / "install-files.txt").open("a", encoding="utf-8") as inventory:
        inventory.write("plugin|Contents/Resources/models/old-bundled.pth\n")
    run(second, "upgrade-remove-only-tracked-singer-model")
    assert not retired.exists() and imported.read_text() == "user import"
    uninstall("uninstall-after-singer-model-removal")
    assert not APP.exists() and imported.exists()
    remove(PLUGIN)

    # Machine-wide binaries use private per-user caches; removal is marker-guarded.
    run(second, "install-for-private-profile-cache-cleanup")
    def profile_state(user):
        return SANDBOX / "profiles" / user / "AppData/Local/Taketo/RvcFeatureInjector/state"
    profile_a, profile_b, unknown = (profile_state(name) for name in ("用户 A", "User B", "Unowned"))
    marker = "TAKETO-RVCFEATUREINJECTOR-STATE-V1\n"
    for state in (profile_a, profile_b):
        state.mkdir(parents=True, exist_ok=True)
        (state / ".taketo-rvc-state").write_bytes(marker.encode("ascii"))
        write(state / "cache/generated.bin", "temporary inference cache")
    write(unknown / ".taketo-rvc-state", "different owner")
    write(unknown / "keep.txt", "unowned state must survive")
    handle = kernel.CreateFileW(str(profile_b / "cache/generated.bin"), 0x80000000, 1, None, 3, 0, None)
    assert handle != wintypes.HANDLE(-1).value
    try:
        uninstall("reject-locked-private-profile-cache", success=False)
        assert APP.exists() and profile_a.exists() and profile_b.exists()
    finally:
        kernel.CloseHandle(handle)
    link = profile_a / "linked"
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)], check=True, capture_output=True)
    try:
        uninstall("reject-junction-private-profile-cache", success=False)
        assert APP.exists() and (outside / "keep.txt").exists()
    finally:
        os.rmdir(link)
    uninstall("uninstall-owned-private-profile-caches")
    assert_clean()
    assert not profile_a.exists() and not profile_b.exists()
    assert (unknown / "keep.txt").read_text() == "unowned state must survive"
    assert (outside / "keep.txt").read_text() == "outside sentinel"

    run(second, "install-for-cleanup-tool-integrity")
    helper = APP / "rvc-state-cleanup.exe"
    original_helper = helper.read_bytes()
    helper.write_bytes(original_helper + b"unexpected modification")
    uninstall("reject-modified-cleanup-tool", success=False)
    assert APP.exists() and PLUGIN.exists()
    helper.write_bytes(original_helper)
    uninstall("uninstall-after-cleanup-tool-repair")
    assert_clean()

    # A changed helper hash must not strand the old uninstaller after failed upgrade.
    helper_a, helper_b = BASE / "helper-a.exe", BASE / "helper-b.exe"
    helper_a.write_bytes(original_helper + b"fixture helper version A")
    helper_b.write_bytes(original_helper + b"fixture helper version B")
    version_a = compile_version("0.0.3", helper_a)
    version_b = compile_version("0.0.4", helper_b)
    run(version_a, "install-helper-version-a")
    assert (APP / "rvc-state-cleanup.exe").read_bytes() == helper_a.read_bytes()
    old_inventory = (APP / "install-files.txt").read_bytes()
    old_uninstall = (APP / "unins000.dat").read_bytes()
    run(version_b, "failed-upgrade-restore-previous-helper", "/FixtureAbort=1", success=False)
    assert (APP / "rvc-state-cleanup.exe").read_bytes() == helper_a.read_bytes()
    assert (APP / "install-files.txt").read_bytes() == old_inventory
    assert (APP / "unins000.dat").read_bytes() == old_uninstall
    uninstall("uninstall-after-failed-helper-upgrade")
    assert_clean()
    run(version_a, "reinstall-helper-version-a")
    run(version_b, "upgrade-to-helper-version-b")
    assert (APP / "rvc-state-cleanup.exe").read_bytes() == helper_b.read_bytes()
    uninstall("uninstall-after-successful-helper-upgrade")
    assert_clean()
    (BASE / "results.json").write_text(json.dumps(RESULTS, indent=2), encoding="utf-8")
    print(json.dumps({"passed": len(RESULTS), "results": str(BASE / "results.json")}), flush=True)


if __name__ == "__main__":
    main()
