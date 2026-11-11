"""Build the VST3 and its inference support installer with a portable Inno compiler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'^#define RVC_PLUGIN_VERSION "([^"]+)"',
                    (ROOT / "src/PluginVersion.h").read_text(encoding="utf-8"), re.MULTILINE).group(1)


def tracked(source):
    return sorted(p for p in source.rglob("*") if p.is_file() and "__pycache__" not in p.parts
                  and p.suffix not in (".pyc", ".pyo") and ".rvc-state" not in p.parts)


def inno_quote(value):
    return '"' + value.replace('"', '""').replace('{', '{{') + '"'


def write_runtime_rules(manifest, runtime, runtime_files, entries):
    rules = ["; Generated from the exact runtime payload; compare content, never timestamps.", "[Files]"]
    for path in runtime_files:
        relative = path.relative_to(runtime).as_posix().replace("/", "\\")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        destination = "{app}\\rvc-runtime"
        if path.parent != runtime:
            destination += "\\" + path.parent.relative_to(runtime).as_posix().replace("/", "\\").replace("{", "{{")
        literal = relative.replace("'", "''")
        rules.append(f'Source: {inno_quote(str(path))}; DestDir: "{destination}"; '
                     f"Flags: ignoreversion; Check: RuntimeFileNeedsUpdate('{literal}', '{digest}')")
    runtime_dirs = [path.relative_to(runtime).as_posix().replace("/", "\\")
                    for path in runtime.rglob("*") if path.is_dir()
                    and "__pycache__" not in path.parts and ".rvc-state" not in path.parts]
    rules += ["[Dirs]", 'Name: "{app}\\rvc-runtime"']
    rules.extend('Name: "{app}\\rvc-runtime\\' + name.replace("{", "{{") + '"' for name in runtime_dirs)
    (manifest / "runtime-files.iss").write_text("\n".join(rules) + "\n", encoding="utf-8-sig")

    # Check:false does not write Inno's ordinary uninstall log. Explicit, nonrecursive
    # entries retain ownership even when a repair had to recreate a lost uninstall log.
    uninstall = ["; Exact owned names only; never recursively delete user additions.", "[UninstallDelete]"]
    directories = {"{app}", "{code:GetPluginDir}", "{app}\\rvc-runtime"}
    directories.update("{app}\\rvc-runtime\\" + name.replace("{", "{{") for name in runtime_dirs)
    for entry in sorted(entries | {"app|unins000.exe", "app|unins000.dat"}):
        kind, relative = entry.split("|", 1)
        root = "{app}" if kind == "app" else "{code:GetPluginDir}"
        components = relative.replace("/", "\\").replace("{", "{{").split("\\")
        uninstall.append(f'Type: files; Name: "{root}\\' + "\\".join(components) + '"')
        for count in range(1, len(components)):
            directories.add(root + "\\" + "\\".join(components[:count]))
    # Inno executes UninstallDelete entries in script order, so children precede parents.
    uninstall.extend(f'Type: dirifempty; Name: "{name}"'
                     for name in sorted(directories, key=lambda name: (-name.count("\\"), name)))
    (manifest / "owned-uninstall.iss").write_text("\n".join(uninstall) + "\n", encoding="utf-8-sig")


def write_release_metadata(output, version, file_count):
    output = output.resolve()
    files = sorted(output.glob(f"RvcFeatureInjector-{version}-Setup*"))
    expected = output / f"RvcFeatureInjector-{version}-Setup.exe"
    if files != [expected] or expected.stat().st_size >= 4_200_000_000:
        raise RuntimeError("Release must contain one self-contained Setup.exe below Inno's 4,200,000,000-byte limit")
    checksums = []
    for path in files:
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksums.append(f"{digest}  {path.name}")
    (output / "SHA256SUMS.txt").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    (output / "INSTALLATION.txt").write_bytes((ROOT / "installer/install-info.txt").read_bytes())
    (output / "README.md").write_bytes((ROOT / "installer/README.md").read_bytes())
    (output / "RELEASE_NOTES.md").write_bytes((ROOT / "installer/RELEASE_NOTES.md").read_bytes())
    (output / "RELEASE.json").write_text(json.dumps({
        "version": version, "files": file_count, "compiler": "Inno Setup 7.1.0 portable",
        "setup_files": [{"name": path.name, "bytes": path.stat().st_size} for path in files],
        "publisher": "Taketo", "singer_models_bundled": False,
        "distribution": "single self-contained offline executable; no external volumes or downloads",
        "compression": "lzma2/max; non-solid for per-file runtime reuse",
        "compression_block_threads": 2,
        "install_scope": "all users", "requires_admin": True,
        "upgrade": "Run this Setup over 1.1.3 or later; reuse both existing installation directories",
        "default_vst3_parent": r"C:\Program Files\Common Files\VST3",
        "support_files": "Portable RVC inference runtime, base assets, configuration and uninstaller; no standalone app",
        "uninstall": "tracked files plus dedicated state directory; preserve user additions",
        "runtime_updates": "SHA256 content comparison; unchanged files skip extraction and writes; missing or changed files repaired",
    }, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--payload-root", type=Path, default=ROOT / "dist/RvcFeatureInjector-1.1.0-portable")
    parser.add_argument("--plugin-root", type=Path, default=ROOT / "dist/RvcFeatureInjector.vst3")
    parser.add_argument("--cleanup-helper", type=Path, default=ROOT / "build/Release/rvc-state-cleanup.exe")
    parser.add_argument("--test-root", type=Path)
    parser.add_argument("--version", default=VERSION)
    parser.add_argument("--output-dir", type=Path, default=ROOT / f"dist/installer-{VERSION}")
    args = parser.parse_args()
    payload, plugin = args.payload_root.resolve(), args.plugin_root.resolve()
    cleanup_helper = args.cleanup_helper.resolve()
    for required in (payload / "rvc-runtime/runtime/python.exe",
                     plugin / "Contents/x86_64-win/RvcFeatureInjector.vst3",
                     plugin / "Contents/Resources/tools/rvc_daemon_worker.py", cleanup_helper):
        if not required.is_file():
            raise SystemExit(f"Missing installer input: {required}")
    if not args.test_root:
        metadata = json.loads((plugin / "Contents/Resources/moduleinfo.json").read_text(encoding="utf-8"))
        if metadata.get("Version") != args.version or any(
                entry.get("Version") != args.version for entry in metadata.get("Classes", [])):
            raise SystemExit("The built plug-in moduleinfo version does not match the installer version; rebuild the plug-in first.")
        for asset in ("hubert_base/config.json", "hubert_base/preprocessor_config.json",
                      "hubert_base/pytorch_model.bin", "rmvpe/rmvpe.pt"):
            if not (payload / "rvc-runtime/assets" / asset).is_file():
                raise SystemExit(f"Missing base asset: {asset}")
        for library in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
            if not (payload / "rvc-runtime/runtime" / library).is_file():
                raise SystemExit(f"Missing app-local VC++ runtime: {library}")
    manifest = ROOT / "build/installer" / ("test" if args.test_root else "release")
    if args.test_root:
        fixture_key = str(args.test_root.resolve()) + "|" + str(plugin)
        manifest = manifest / hashlib.sha256(fixture_key.encode("utf-8")).hexdigest()[:16]
    manifest.mkdir(parents=True, exist_ok=True)
    entries = {"app|INSTALLATION.txt", "app|install-files.txt", "app|.rvc-install-owner", "app|plugin-root.txt",
               "app|rvc-state-cleanup.exe"}
    runtime_files = tracked(payload / "rvc-runtime")
    for path in runtime_files:
        entries.add("app|rvc-runtime/" + path.relative_to(payload / "rvc-runtime").as_posix())
    for path in tracked(plugin):
        relative = path.relative_to(plugin)
        if (relative.parts[:3] != ("Contents", "Resources", "models") and
                path.name not in ("runtime-root.txt", "state-root.txt", "install-owner.txt")):
            entries.add("plugin|" + path.relative_to(plugin).as_posix())
    for name in ("runtime-root.txt", "state-root.txt", "install-owner.txt"):
        entries.add("plugin|Contents/Resources/" + name)
    (manifest / "install-files.txt").write_text("\n".join(sorted(entries)) + "\n", encoding="utf-8-sig")
    write_runtime_rules(manifest, payload / "rvc-runtime", runtime_files, entries)
    compiler = ROOT / ".tools/innosetup/app/ISCC.exe"
    if not compiler.is_file():
        raise SystemExit("Run tools/bootstrap_installer.py first.")
    command = [str(compiler), f"/DPayloadRoot={payload}", f"/DPluginRoot={plugin}",
               f"/DManifestRoot={manifest}", f"/DReleaseVersion={args.version}", f"/O{args.output_dir.resolve()}"]
    command += [f"/DCleanupHelper={cleanup_helper}",
                f"/DCleanupHelperSha256={hashlib.sha256(cleanup_helper.read_bytes()).hexdigest()}"]
    if args.test_root:
        command.append(f"/DTestRoot={args.test_root.resolve()}")
    temp = manifest / "temp"
    temp.mkdir(exist_ok=True)
    env = {key.upper(): value for key, value in os.environ.items()}
    env.update(TEMP=str(temp), TMP=str(temp))
    subprocess.run(command + [str(ROOT / "installer/RvcFeatureInjector.iss")], check=True, cwd=ROOT, env=env)
    if not args.test_root:
        write_release_metadata(args.output_dir, args.version, len(entries))


if __name__ == "__main__":
    main()
