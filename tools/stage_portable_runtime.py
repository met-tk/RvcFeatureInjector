"""Copy the existing portable RVC runtime into a release directory; no installation."""
import argparse
from pathlib import Path
import shutil

def copy_tree(source, target):
    shutil.copytree(source, target, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"))


def copy_crt(source, target):
    """Deploy the trusted x64 Microsoft.VC143.CRT redistributable beside python.exe."""
    source = Path(source).resolve()
    required = ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
    if any(not (source / name).is_file() for name in required):
        raise ValueError("--crt-root must point to the x64 Microsoft.VC143.CRT DLL directory")
    target = Path(target)
    target.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.glob("*.dll")):
        shutil.copy2(path, target / path.name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--crt-root", type=Path, help="Optional trusted x64 Microsoft.VC143.CRT DLL directory for app-local deployment")
    args = parser.parse_args()
    source, output = args.runtime_root.resolve(), args.output.resolve()
    if source == output or source in output.parents:
        raise SystemExit("The release must be outside the source runtime.")
    runtime = output / "rvc-runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    for name in ("runtime", "infer", "configs"):
        print(f"Copying {name}...", flush=True)
        copy_tree(source / name, runtime / name)
    if args.crt_root:
        copy_crt(args.crt_root, runtime / "runtime")
    (runtime / "tools").mkdir(exist_ok=True)
    shutil.copy2(source / "tools/cuda_graph.py", runtime / "tools/cuda_graph.py")
    shutil.copy2(source / "tools/file_io.py", runtime / "tools/file_io.py")
    copy_tree(source / "assets/hubert_base", runtime / "assets/hubert_base")
    (runtime / "assets/rmvpe").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "assets/rmvpe/rmvpe.pt", runtime / "assets/rmvpe/rmvpe.pt")
    shutil.copy2(source / "LICENSE", runtime / "RVC-LICENSE.txt")
    extra_license = source / "MIT协议暨相关引用库协议"
    if extra_license.is_file():
        shutil.copy2(extra_license, runtime / "THIRD-PARTY-NOTICES.txt")
    print(f"Portable runtime staged: {output}", flush=True)


if __name__ == "__main__":
    main()
