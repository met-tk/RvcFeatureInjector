"""Verify content-based runtime reuse with actual, registry-free project-only installers."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "artifacts/installer-1.1.9-2026-11-11"
BASE = ART / "incremental-fixture"
PAYLOAD = BASE / "payload"
SANDBOX = BASE / "中文 增量沙盒"
APP = SANDBOX / "app/RvcFeatureInjector"
PLUGIN = SANDBOX / "VST3/RvcFeatureInjector.vst3"
RUNTIME = APP / "rvc-runtime"
SOURCE_ARCHIVE = ROOT / "dist/releases/RvcFeatureInjector-1.1.4-source.zip"
FIXED_TIME_NS = 1_600_000_000_000_000_000
SKIP_MARKER = "RVC runtime unchanged, skipped:"
RESULTS = []
EVIDENCE = {}


class FileInformation(ctypes.Structure):
    _fields_ = [("attributes", wintypes.DWORD), ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME), ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]


def kernel_api():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(FileInformation)]
    kernel.GetFileInformationByHandle.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    return kernel


def file_state(path):
    info = FileInformation()
    kernel = kernel_api()
    handle = kernel.CreateFileW(str(path), 0x80, 7, None, 3, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)
    ticks = lambda stamp: (stamp.dwHighDateTime << 32) | stamp.dwLowDateTime
    return {"file_id": f"{info.volume:08x}:{info.index_high:08x}{info.index_low:08x}",
            "creation_ticks": ticks(info.creation), "write_ticks": ticks(info.write),
            "size": (info.size_high << 32) | info.size_low,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    os.utime(path, ns=(FIXED_TIME_NS, FIXED_TIME_NS))


def remove_generated(path):
    resolved = path.resolve()
    assert resolved.is_relative_to(BASE.resolve()) and resolved != BASE.resolve(), resolved
    if path.exists():
        shutil.rmtree(path)


def source_files():
    return {p.relative_to(PAYLOAD / "rvc-runtime").as_posix(): p
            for p in sorted((PAYLOAD / "rvc-runtime").rglob("*")) if p.is_file()}


def installed_states(names=None):
    return {name: file_state(RUNTIME / name) for name in (names if names is not None else source_files())}


def run(exe, case, *extra, success=True):
    assert exe.resolve().is_relative_to(BASE.resolve()), "Only project sandbox installer executables are allowed"
    log = BASE / f"{case}.log"
    log.unlink(missing_ok=True)
    command = [str(exe), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", f"/LOG={log}", *extra]
    temporary = BASE / "run-temp"
    temporary.mkdir(exist_ok=True)
    env = {name.upper(): value for name, value in os.environ.items()}
    env.update(TEMP=str(temporary), TMP=str(temporary))
    result = subprocess.run(command, timeout=120, env=env)
    # Inno's uninstaller launches a temporary copy and may return before cleanup has finished.
    for _ in range(180):
        if log.exists() and "Log closed." in log.read_text(encoding="utf-8-sig"):
            break
        time.sleep(0.1)
    content = log.read_text(encoding="utf-8-sig")
    if success:
        assert result.returncode == 0, (case, result.returncode, content[-4000:])
    else:
        assert result.returncode != 0, (case, content[-4000:])
    RESULTS.append({"case": case, "exit_code": result.returncode, "log": str(log), "passed": True})
    return content


def compile_current(label):
    output = BASE / "setups" / label
    with (BASE / f"compile-{label}.log").open("w", encoding="utf-8") as log:
        subprocess.run([sys.executable, str(ROOT / "tools/build_installer.py"),
                        "--payload-root", str(PAYLOAD), "--plugin-root", str(PAYLOAD / PLUGIN.name),
                        "--test-root", str(SANDBOX), "--version", "1.1.8", "--output-dir", str(output)],
                       check=True, stdout=log, stderr=log)
    return output / "RvcFeatureInjector-Test-1.1.8.exe"


def compile_historical():
    assert SOURCE_ARCHIVE.is_file(), SOURCE_ARCHIVE
    history = BASE / "historical-1.1.4"
    history.mkdir(parents=True, exist_ok=True)
    with ZipFile(SOURCE_ARCHIVE) as archive:
        for relative in ("installer/RvcFeatureInjector.iss", "installer/ownership.iss", "installer/install-info.txt"):
            target = history / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(relative))
    script = history / "installer/RvcFeatureInjector.iss"
    assert '#define ReleaseVersion "1.1.4"' in script.read_text(encoding="utf-8")
    entries = {"app|INSTALLATION.txt", "app|install-files.txt", "app|.rvc-install-owner", "app|plugin-root.txt",
               "app|rvc-state-cleanup.exe"}
    entries.update("app|rvc-runtime/" + name for name in source_files())
    entries.update("plugin|" + p.relative_to(PAYLOAD / PLUGIN.name).as_posix()
                   for p in (PAYLOAD / PLUGIN.name).rglob("*") if p.is_file())
    entries.update("plugin|Contents/Resources/" + name for name in
                   ("runtime-root.txt", "state-root.txt", "install-owner.txt"))
    manifest = history / "manifest"
    manifest.mkdir(exist_ok=True)
    (manifest / "install-files.txt").write_text("\n".join(sorted(entries)) + "\n", encoding="utf-8-sig")
    helper = ROOT / "build/Release/rvc-state-cleanup.exe"
    command = [str(ROOT / ".tools/innosetup/app/ISCC.exe"), f"/DPayloadRoot={PAYLOAD}",
               f"/DPluginRoot={PAYLOAD / PLUGIN.name}", f"/DManifestRoot={manifest}",
               f"/DTestRoot={SANDBOX}", f"/DCleanupHelper={helper}",
               f"/DCleanupHelperSha256={hashlib.sha256(helper.read_bytes()).hexdigest()}",
               f"/O{BASE / 'setups/historical-1.1.4'}", str(script)]
    temporary = history / "temp"
    temporary.mkdir(exist_ok=True)
    env = {name.upper(): value for name, value in os.environ.items()}
    env.update(TEMP=str(temporary), TMP=str(temporary))
    with (BASE / "compile-historical-1.1.4.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=log, check=True)
    EVIDENCE["historical_source"] = {
        "archive": str(SOURCE_ARCHIVE), "sha256": hashlib.sha256(SOURCE_ARCHIVE.read_bytes()).hexdigest(),
        "installer_script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(),
        "ownership_script_sha256": hashlib.sha256((history / "installer/ownership.iss").read_bytes()).hexdigest()}
    return BASE / "setups/historical-1.1.4/RvcFeatureInjector-Test-1.1.4.exe"


def assert_reused(before, log, label):
    after = installed_states(before)
    lines = [line for line in log.splitlines() if SKIP_MARKER.lower() in line.lower()]
    for name, original in before.items():
        assert after[name] == original, (label, name, original, after[name])
        assert any(name.replace("/", "\\").lower() in line.replace("/", "\\").lower() for line in lines), (label, name, "skip evidence missing")
    EVIDENCE[label] = {"before": before, "after": after, "skip_log_lines": lines}


def assert_payload():
    for name, source in source_files().items():
        assert (RUNTIME / name).read_bytes() == source.read_bytes(), name


def find_uninstaller():
    candidates = [exe for exe in APP.glob("unins*.exe") if exe.with_suffix(".dat").is_file()]
    assert candidates, "Repair did not recreate a usable uninstaller"
    return max(candidates, key=lambda exe: (exe.with_suffix(".dat").stat().st_mtime_ns, exe.name))


def remove_uninstall_history(remove_executable=False):
    for dat in APP.glob("unins*.dat"):
        dat.unlink()
    if remove_executable:
        for exe in APP.glob("unins*.exe"):
            exe.unlink()


def uninstall(case):
    return run(find_uninstaller(), case)


def assert_clean():
    assert not APP.exists(), [str(p) for p in APP.rglob("*")] if APP.exists() else ""
    assert not PLUGIN.exists(), [str(p) for p in PLUGIN.rglob("*")] if PLUGIN.exists() else ""


def stamp_installed():
    # Deliberately differ from the archive: ordinary overwrite would reset this timestamp.
    for number, name in enumerate(source_files()):
        if not (RUNTIME / name).is_file():
            continue
        stamp = FIXED_TIME_NS + (number + 1) * 86_400_000_000_000
        os.utime(RUNTIME / name, ns=(stamp, stamp))
        assert file_state(RUNTIME / name)["write_ticks"] != file_state(PAYLOAD / "rvc-runtime" / name)["write_ticks"]


def main():
    BASE.mkdir(parents=True, exist_ok=True)
    assert not APP.exists() and not PLUGIN.exists(), "Uninstall the previous sandbox install first"
    remove_generated(PAYLOAD)
    for name, content in {"runtime/python.exe": b"portable python fixture", "runtime/unchanged.bin": b"unchanged runtime",
                          "runtime/corrupt.bin": b"ORIGINAL", "runtime/missing.bin": b"missing file fixture",
                          "runtime/changed.bin": b"before-1", "runtime/obsolete.bin": b"retire this runtime file",
                          "assets/hubert_base/pytorch_model.bin": b"base asset fixture"}.items():
        write(PAYLOAD / "rvc-runtime" / name, content)
    write(PAYLOAD / PLUGIN.name / "Contents/x86_64-win/RvcFeatureInjector.vst3", b"native plugin fixture")
    write(PAYLOAD / PLUGIN.name / "Contents/Resources/tools/rvc_daemon_worker.py", b"worker fixture")
    historical = compile_historical()
    current = compile_current("unchanged")

    run(historical, "install-original-1.1.4")
    assert_payload()
    stamp_installed()
    before = installed_states()
    log = run(current, "upgrade-original-to-1.1.8-content-reuse")
    assert_reused(before, log, "real_1_1_4_to_1_1_6")
    log = run(current, "same-version-repair-content-reuse")
    assert_reused(before, log, "same_version_repair")

    corrupt = RUNTIME / "runtime/corrupt.bin"
    corrupt.write_bytes(b"CORRUPT!")
    source_corrupt = PAYLOAD / "rvc-runtime/runtime/corrupt.bin"
    os.utime(corrupt, ns=(source_corrupt.stat().st_atime_ns, source_corrupt.stat().st_mtime_ns))
    assert corrupt.stat().st_size == source_corrupt.stat().st_size
    assert file_state(corrupt)["write_ticks"] == file_state(source_corrupt)["write_ticks"]
    EVIDENCE["same_size_same_time_corruption"] = {"corrupt": file_state(corrupt), "source": file_state(source_corrupt)}
    missing = RUNTIME / "runtime/missing.bin"
    missing.unlink()
    survivors = installed_states(name for name in source_files() if name not in ("runtime/corrupt.bin", "runtime/missing.bin"))
    log = run(current, "repair-same-size-same-time-corruption-and-missing")
    assert_payload()
    assert_reused(survivors, log, "repair_preserves_healthy_files")

    unknown = RUNTIME / "user-imports/my-model.pth"
    write(unknown, b"user-owned sentinel")
    unknown_state = file_state(unknown)
    write(PAYLOAD / "rvc-runtime/runtime/changed.bin", b"after--2")
    (PAYLOAD / "rvc-runtime/runtime/obsolete.bin").unlink()
    write(PAYLOAD / "rvc-runtime/runtime/new.bin", b"new runtime fixture")
    updated = compile_current("changed-payload")
    survivors = installed_states(name for name in source_files() if name not in ("runtime/changed.bin", "runtime/new.bin"))
    log = run(updated, "update-changed-new-and-obsolete-runtime")
    assert_payload()
    assert not (RUNTIME / "runtime/obsolete.bin").exists()
    assert file_state(unknown) == unknown_state
    assert_reused(survivors, log, "changed_payload_preserves_healthy_files")
    uninstall("uninstall-after-incremental-updates-preserve-user-file")
    assert unknown.read_bytes() == b"user-owned sentinel"
    assert {p for p in APP.rglob("*") if p.is_file()} == {unknown}
    assert not PLUGIN.exists()
    remove_generated(APP)

    run(updated, "fresh-1.1.8-install")
    assert_payload()
    stamp_installed()
    before = installed_states()
    log = run(updated, "fresh-1.1.8-repair-reuses-runtime")
    assert_reused(before, log, "fresh_install_then_repair")
    # Losing the uninstall history must not strand files skipped by content comparison.
    remove_uninstall_history()
    log = run(updated, "repair-missing-uninstall-history")
    assert_reused(before, log, "missing_uninstall_history_repair")
    assert find_uninstaller().with_suffix(".dat").is_file()
    uninstall("uninstall-repair-without-old-history")
    assert_clean()

    run(updated, "install-for-missing-uninstaller-and-history")
    stamp_installed()
    before = installed_states()
    remove_uninstall_history(remove_executable=True)
    log = run(updated, "repair-missing-uninstaller-and-history")
    assert_reused(before, log, "missing_uninstaller_and_history_repair")
    log = run(updated, "repeat-repair-after-recreated-uninstaller")
    assert_reused(before, log, "repeat_repair_after_recreated_uninstaller")
    uninstall("uninstall-after-recreated-uninstaller")
    assert_clean()

    run(historical, "install-original-for-failed-upgrade")
    stamp_installed()
    old_inventory = (APP / "install-files.txt").read_bytes()
    before = installed_states(name for name in source_files() if name not in ("runtime/changed.bin", "runtime/new.bin"))
    log = run(updated, "fail-upgrade-after-runtime-copy", "/FixtureAbort=1", success=False)
    assert_reused(before, log, "failed_upgrade_reuses_healthy_runtime")
    assert (APP / "install-files.txt").read_bytes() == old_inventory
    assert not (RUNTIME / "runtime/new.bin").exists()
    assert (RUNTIME / "runtime/obsolete.bin").is_file()
    uninstall("uninstall-directly-after-failed-upgrade")
    assert_clean()

    run(updated, "final-clean-install")
    stamp_installed()
    before = installed_states()
    log = run(updated, "final-all-runtime-skipped-repair")
    assert_reused(before, log, "final_all_skipped")
    uninstall("final-clean-uninstall-includes-skipped-runtime")
    assert_clean()


if __name__ == "__main__":
    status = "failed"
    try:
        main()
        status = "passed"
        print(json.dumps({"passed": len(RESULTS), "results": str(BASE / "results.json")}), flush=True)
    finally:
        BASE.mkdir(parents=True, exist_ok=True)
        (BASE / "results.json").write_text(json.dumps({"status": status, "runs": RESULTS, "evidence": EVIDENCE},
                                                       indent=2), encoding="utf-8")
