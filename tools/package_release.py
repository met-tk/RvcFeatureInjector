"""Stage and archive already-built release bundles, including portable runtime if present."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parent.parent
VERSION = re.search(r'^#define RVC_PLUGIN_VERSION "([^"]+)"',
                    (ROOT / "src/PluginVersion.h").read_text(encoding="utf-8"), re.MULTILINE).group(1)


def stage(kind):
    target = ROOT / "dist" / f"RvcFeatureInjector-{VERSION}-{kind}"
    bundle = target / "RvcFeatureInjector.vst3"
    shutil.copytree(ROOT / "dist/RvcFeatureInjector.vst3", bundle, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("models", "__pycache__", "*.pyc", "*.pyo"))
    config = bundle / "Contents/Resources/runtime-root.txt"
    config.write_text("../../../rvc-runtime\n" if kind == "portable" else "D:\\RVC20260718Nvidia\n", encoding="utf-8")
    shutil.copy2(ROOT / "README.md", target / "README.md")
    shutil.copy2(ROOT / "tools/Install-UserPlugin.ps1", target / "Install-UserPlugin.ps1")
    notes = ROOT / "installer/RELEASE_NOTES.md"
    if notes.is_file():
        shutil.copy2(notes, target / "RELEASE_NOTES.md")
    if kind == "portable" and not (target / "rvc-runtime/runtime/python.exe").is_file():
        raise SystemExit("Run stage_portable_runtime.py first.")
    return target


def archive(folder):
    destination = ROOT / "dist/releases" / (folder.name + ".zip")
    destination.parent.mkdir(exist_ok=True)
    files = sorted(p for p in folder.rglob("*") if p.is_file() and "__pycache__" not in p.parts
                   and p.suffix not in (".pyc", ".pyo")
                   and p.relative_to(folder).parts[:4] != ("RvcFeatureInjector.vst3", "Contents", "Resources", "models"))
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as output:
        for number, path in enumerate(files, 1):
            output.write(path, path.relative_to(folder.parent))
            if number % 5000 == 0:
                print(f"{folder.name}: {number}/{len(files)} files", flush=True)
    with zipfile.ZipFile(destination) as check:
        failure = check.testzip()
        if failure:
            raise RuntimeError(f"Corrupt archive member: {failure}")
    with destination.open("rb") as content:
        digest = hashlib.file_digest(content, "sha256").hexdigest()
    destination.with_suffix(".zip.sha256").write_text(f"{digest}  {destination.name}\n", encoding="utf-8")
    print(json.dumps({"archive": str(destination), "files": len(files), "bytes": destination.stat().st_size, "sha256": digest, "crc_verified": True}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--stage-only", action="store_true")
    mode.add_argument("--source-only", action="store_true", help="Archive source without copying or archiving the portable runtime")
    args = parser.parse_args()
    if not args.source_only:
        for kind in ("plugin", "portable"):
            folder = stage(kind)
            if not args.stage_only:
                archive(folder)
    if not args.stage_only:
        (ROOT / "dist/releases").mkdir(exist_ok=True)
        source = ROOT / "dist/releases" / f"RvcFeatureInjector-{VERSION}-source.zip"
        files = [ROOT / name for name in ("README.md", "PROJECT_CONTEXT.md", "CMakeLists.txt", "install_to_cubase.bat")]
        for directory in ("src", "tools", "tests", "installer"):
            files.extend(p for p in (ROOT / directory).rglob("*") if p.is_file() and "__pycache__" not in p.parts)
        with zipfile.ZipFile(source, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as output:
            for path in sorted(files):
                output.write(path, path.relative_to(ROOT))
        with source.open("rb") as content:
            digest = hashlib.file_digest(content, "sha256").hexdigest()
        source.with_suffix(".zip.sha256").write_text(f"{digest}  {source.name}\n", encoding="utf-8")
        print(f"Source: {source.name} SHA256={digest}", flush=True)
