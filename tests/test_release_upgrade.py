"""Upgrade real previous releases to the current version in project-only sandboxes."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CURRENT_VERSION = re.search(r'^#define RVC_PLUGIN_VERSION "([^"]+)"',
                            (ROOT / "src/PluginVersion.h").read_text(encoding="utf-8"), re.MULTILINE).group(1)
PREVIOUS_RELEASES = {
    "1.1.7": {"extracted": ROOT / "artifacts/installer-1.1.7-2026-11-11/extracted-final",
              "source_sha256": "f639757e24f8ef6eaf7d44f80da75e6680d2e6f002ab0a1dc40bbf5c688a8983"},
    "1.1.8": {"extracted": ROOT / "artifacts/installer-1.1.8-2026-11-11/extracted-final",
              "source_sha256": "a25805477937a54fb0b124fa51f7e68269604d15b3af9222eed2f72e89c59f26"},
}
BASE = OLD = SANDBOX = APP = VST_PARENT = PLUGIN = None
OLD_VERSION = SOURCE_SHA256 = None
DLL = Path("Contents/x86_64-win/RvcFeatureInjector.vst3")
RESOURCE = Path("Contents/Resources")
RESULTS = []


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, content=b"fixture"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def remove(path):
    assert path.resolve().is_relative_to(BASE.resolve()) and path.resolve() != BASE.resolve()
    if path.exists():
        shutil.rmtree(path)


def run(exe, name, *extra, success=True):
    log = BASE / f"{name}.log"
    log.unlink(missing_ok=True)
    result = subprocess.run([str(exe), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                             "/SP-", f"/LOG={log}", *extra], timeout=90)
    # Uninstall's launcher exits before its temporary copy finishes.
    for _ in range(120):
        if log.exists() and "Log closed." in log.read_text(encoding="utf-8-sig"):
            break
        time.sleep(0.1)
    text = log.read_text(encoding="utf-8-sig")
    assert (result.returncode == 0) == success, (name, result.returncode, text[-3500:])
    RESULTS.append({"case": name, "exit_code": result.returncode, "passed": True})


def stage(version, source):
    payload = BASE / f"payload-{version}"
    remove(payload)
    plugin = payload / "RvcFeatureInjector.vst3"
    for relative in (DLL, RESOURCE / "moduleinfo.json", RESOURCE / "tools/rvc_daemon_worker.py"):
        target = plugin / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, target)
    write(payload / "rvc-runtime/runtime/python.exe", b"representative runtime fixture")
    return payload, plugin


def compile_old(payload, plugin):
    archive = ROOT / f"dist/releases/RvcFeatureInjector-{OLD_VERSION}-source.zip"
    assert digest(archive) == SOURCE_SHA256, f"The original {OLD_VERSION} source archive changed"
    previous = BASE / f"original-{OLD_VERSION}"
    previous.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as source:
        members = ["installer/RvcFeatureInjector.iss", "installer/ownership.iss", "installer/install-info.txt", "tools/build_installer.py"]
        if "src/PluginVersion.h" in source.namelist():
            members.append("src/PluginVersion.h")
        for name in members:
            write(previous / name, source.read(name))
    old_script = (previous / "installer/RvcFeatureInjector.iss").read_text(encoding="utf-8")
    current_script = (ROOT / "installer/RvcFeatureInjector.iss").read_text(encoding="utf-8")
    for setting in ("AppId=Taketo.RvcFeatureInjector.Machine", "UsePreviousAppDir=yes", "PrivilegesRequired=admin"):
        assert setting in old_script and setting in current_script, setting
    assert f'ReleaseVersion "{OLD_VERSION}"' in old_script
    assert f'ReleaseVersion "{CURRENT_VERSION}"' in current_script
    manifest = previous / "manifest"
    manifest.mkdir(exist_ok=True)
    entries = {"app|.rvc-install-owner", "app|INSTALLATION.txt", "app|install-files.txt",
               "app|plugin-root.txt", "app|rvc-state-cleanup.exe", "app|rvc-runtime/runtime/python.exe"}
    entries.update("plugin|" + p.relative_to(plugin).as_posix() for p in plugin.rglob("*") if p.is_file())
    entries.update("plugin|Contents/Resources/" + n for n in ("runtime-root.txt", "state-root.txt", "install-owner.txt"))
    (manifest / "install-files.txt").write_text("\n".join(sorted(entries)) + "\n", encoding="utf-8-sig")
    spec = importlib.util.spec_from_file_location("original_installer_builder", previous / "tools/build_installer.py")
    old_builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(old_builder)
    runtime = payload / "rvc-runtime"
    old_builder.write_runtime_rules(manifest, runtime, old_builder.tracked(runtime), entries)
    helper = OLD / "{app}/rvc-state-cleanup.exe"
    temp = previous / "temp"
    temp.mkdir(exist_ok=True)
    env = {key.upper(): value for key, value in os.environ.items()}
    env.update(TEMP=str(temp), TMP=str(temp))
    command = [str(ROOT / ".tools/innosetup/app/ISCC.exe"), f"/DPayloadRoot={payload}",
               f"/DPluginRoot={plugin}", f"/DManifestRoot={manifest}", f"/DReleaseVersion={OLD_VERSION}",
               f"/DCleanupHelper={helper}", f"/DCleanupHelperSha256={digest(helper)}",
               f"/DTestRoot={SANDBOX}", f"/O{BASE / 'setups'}", str(previous / "installer/RvcFeatureInjector.iss")]
    with (BASE / f"compile-original-{OLD_VERSION}.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, check=True, env=env, stdout=log, stderr=log)
    return BASE / f"setups/RvcFeatureInjector-Test-{OLD_VERSION}.exe"


def main():
    global BASE, OLD, SANDBOX, APP, VST_PARENT, PLUGIN, OLD_VERSION, SOURCE_SHA256
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-version", choices=tuple(PREVIOUS_RELEASES), default="1.1.8")
    parser.add_argument("--diagnostic-dll", type=Path,
                        help="Also verify direct upgrade from the verified 1.1.8 DIAG1 DLL")
    args = parser.parse_args()
    OLD_VERSION = args.old_version
    diagnostic_sha256 = None
    if args.diagnostic_dll:
        assert OLD_VERSION == "1.1.8", "DIAG1 was built from 1.1.8"
        diagnostic_sha256 = digest(args.diagnostic_dll)
        assert diagnostic_sha256 == "c87b15eba16ceeab3e0a981eff5eea70a046e752141309b9041952affc95c28a"
    assert CURRENT_VERSION != OLD_VERSION, "Build a newer current release before exercising upgrades"
    previous = PREVIOUS_RELEASES[OLD_VERSION]
    OLD = previous["extracted"]
    SOURCE_SHA256 = previous["source_sha256"]
    BASE = ROOT / f"artifacts/release-upgrade/{OLD_VERSION}-to-{CURRENT_VERSION}"
    SANDBOX = BASE / "中文 升级沙盒"
    APP = SANDBOX / "自选 推理库/RvcFeatureInjector"
    VST_PARENT = SANDBOX / "自选 VST3"
    PLUGIN = VST_PARENT / "RvcFeatureInjector.vst3"
    RESULTS.clear()
    BASE.mkdir(parents=True, exist_ok=True)
    assert not APP.exists() and not PLUGIN.exists(), "Uninstall the prior fixture before rerunning"
    remove(SANDBOX)
    old_payload, old_plugin = stage(OLD_VERSION, OLD / "{code_GetPluginDir}")
    new_payload, new_plugin = stage(CURRENT_VERSION, ROOT / "dist/RvcFeatureInjector.vst3")
    old_info = json.loads((old_plugin / RESOURCE / "moduleinfo.json").read_text(encoding="utf-8"))
    new_info = json.loads((new_plugin / RESOURCE / "moduleinfo.json").read_text(encoding="utf-8"))
    assert old_info["Version"] == OLD_VERSION and new_info["Version"] == CURRENT_VERSION
    assert [entry["CID"] for entry in old_info["Classes"]] == [entry["CID"] for entry in new_info["Classes"]]
    assert all(entry["Version"] == CURRENT_VERSION and entry["Vendor"] == "Taketo" for entry in new_info["Classes"])
    assert digest(old_plugin / DLL) != digest(new_plugin / DLL), f"Build the {CURRENT_VERSION} native plug-in first"
    first = compile_old(old_payload, old_plugin)
    with (BASE / f"compile-{CURRENT_VERSION}.log").open("w", encoding="utf-8") as log:
        subprocess.run([sys.executable, str(ROOT / "tools/build_installer.py"),
                        "--payload-root", str(new_payload), "--plugin-root", str(new_plugin),
                        "--test-root", str(SANDBOX), "--output-dir", str(BASE / "setups")],
                       check=True, stdout=log, stderr=log)
    second = BASE / f"setups/RvcFeatureInjector-Test-{CURRENT_VERSION}.exe"
    run(first, f"install-original-{OLD_VERSION}-custom-directories", f"/DIR={APP}", f"/VST3DIR={VST_PARENT}")
    assert digest(PLUGIN / DLL) == digest(old_plugin / DLL)
    old_helper = digest(APP / "rvc-state-cleanup.exe")
    inventory = (APP / "install-files.txt").read_bytes()
    uninstall_data = (APP / "unins000.dat").read_bytes()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(PLUGIN / DLL), 0x80000000, 1, None, 3, 0, None)
    assert handle != wintypes.HANDLE(-1).value
    try:
        run(second, "reject-update-while-host-uses-plugin", f"/DIR={APP}", success=False)
        assert digest(PLUGIN / DLL) == digest(old_plugin / DLL)
    finally:
        kernel.CloseHandle(handle)
    run(second, "failed-update-retains-old-uninstaller-and-helper", f"/DIR={APP}", "/FixtureAbort=1", success=False)
    assert digest(APP / "rvc-state-cleanup.exe") == old_helper
    assert (APP / "unins000.dat").read_bytes() == uninstall_data
    assert (APP / "install-files.txt").read_bytes() == inventory
    run(APP / "unins000.exe", "old-uninstaller-works-after-failed-update")
    assert not APP.exists() and not PLUGIN.exists()
    run(first, f"reinstall-original-{OLD_VERSION}", f"/DIR={APP}", f"/VST3DIR={VST_PARENT}")
    user_model = PLUGIN / RESOURCE / "models/user-import.pth"
    write(user_model, b"preserve imported model")
    private_state = SANDBOX / "profiles/User/AppData/Local/Taketo/RvcFeatureInjector/state"
    write(private_state / ".taketo-rvc-state", b"TAKETO-RVCFEATUREINJECTOR-STATE-V1\n")
    write(private_state / "cache/generated.bin", b"preserve across upgrade")
    run(second, f"upgrade-{OLD_VERSION}-to-{CURRENT_VERSION}-reuses-custom-vst3", f"/DIR={APP}")
    assert digest(PLUGIN / DLL) == digest(new_plugin / DLL)
    assert json.loads((PLUGIN / RESOURCE / "moduleinfo.json").read_text(encoding="utf-8"))["Version"] == CURRENT_VERSION
    assert (APP / "plugin-root.txt").read_text(encoding="utf-8").strip() == str(PLUGIN)
    assert (PLUGIN / RESOURCE / "runtime-root.txt").read_text(encoding="utf-8").strip() == str(APP / "rvc-runtime")
    assert not (SANDBOX / "VST3/RvcFeatureInjector.vst3").exists()
    assert user_model.read_bytes() == b"preserve imported model"
    assert (private_state / "cache/generated.bin").read_bytes() == b"preserve across upgrade"
    assert digest(APP / "rvc-state-cleanup.exe") == digest(ROOT / "build/Release/rvc-state-cleanup.exe")
    run(second, f"repair-{CURRENT_VERSION}-keeps-directory-and-user-files", f"/DIR={APP}")
    assert user_model.read_bytes() == b"preserve imported model"
    run(APP / "unins000.exe", f"uninstall-{CURRENT_VERSION}-after-upgrade")
    assert not APP.exists() and not private_state.exists()
    assert user_model.read_bytes() == b"preserve imported model"
    assert [p for p in PLUGIN.rglob("*") if p.is_file()] == [user_model]
    remove(PLUGIN)
    if args.diagnostic_dll:
        run(first, "install-original-1.1.8-for-diagnostic-upgrade", f"/DIR={APP}", f"/VST3DIR={VST_PARENT}")
        shutil.copyfile(args.diagnostic_dll, PLUGIN / DLL)
        assert digest(PLUGIN / DLL) == diagnostic_sha256
        original_resources = {p.relative_to(PLUGIN).as_posix(): digest(p)
                              for p in (PLUGIN / RESOURCE).rglob("*") if p.is_file()}
        run(second, f"upgrade-1.1.8-DIAG1-directly-to-{CURRENT_VERSION}", f"/DIR={APP}")
        assert digest(PLUGIN / DLL) == digest(new_plugin / DLL)
        assert json.loads((PLUGIN / RESOURCE / "moduleinfo.json").read_text(encoding="utf-8"))["Version"] == CURRENT_VERSION
        for relative in ("runtime-root.txt", "state-root.txt", "install-owner.txt"):
            resource_path = RESOURCE / relative
            assert digest(PLUGIN / resource_path) == original_resources[resource_path.as_posix()]
        run(APP / "unins000.exe", f"uninstall-{CURRENT_VERSION}-after-direct-diagnostic-upgrade")
        assert not APP.exists() and not PLUGIN.exists()
    report = {"passed": len(RESULTS), "old_version": OLD_VERSION, "new_version": CURRENT_VERSION,
              "original_source_sha256": SOURCE_SHA256,
              "plugin_class_ids_unchanged": True,
              "old_native_sha256": digest(old_plugin / DLL), "new_native_sha256": digest(new_plugin / DLL),
              "old_cleanup_sha256": old_helper, "new_cleanup_sha256": digest(ROOT / "build/Release/rvc-state-cleanup.exe"),
              "direct_diagnostic_upgrade_sha256": diagnostic_sha256,
              "cases": RESULTS, "scope": "Project TestRoot, no elevation or uninstall registry entry",
              "runtime_payload": "Representative fixture; native plug-ins/helpers are real release files",
              "directory_test": "Explicit original /DIR (registry disabled); VST3 directory restored automatically",
              "not_executed": "Production UAC, HKLM remembered runtime directory and Cubase GUI"}
    (BASE / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
