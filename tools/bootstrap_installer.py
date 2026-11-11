"""Fetch a pinned portable Inno compiler into this project; never run a setup package."""
import hashlib
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DOWNLOADS = ROOT / ".tools/downloads"
ARCHIVE = DOWNLOADS / "innosetup-portable-win64-7.1.0-7.7z"
URL = "https://github.com/portapps/innosetup-portable/releases/download/7.1.0-6/innosetup-portable-win64-7.1.0-7.7z"
SHA256 = "179cb8d1610430e1b339915cc9ba0082d1493b88f20bd3503918ba666d64ac7e"


if __name__ == "__main__":
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    if not ARCHIVE.is_file():
        with urllib.request.urlopen(URL, timeout=60) as response, ARCHIVE.open("wb") as target:
            while data := response.read(1024 * 1024):
                target.write(data)
    actual = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
    if actual != SHA256:
        raise SystemExit(f"Portable compiler SHA256 mismatch: {actual}")
    output = ROOT / ".tools/innosetup"
    output.mkdir(exist_ok=True)
    subprocess.run(["tar", "-xf", str(ARCHIVE), "-C", str(output)], check=True)
    compilers = list(output.rglob("ISCC.exe"))
    if len(compilers) != 1:
        raise SystemExit(f"Expected one compiler, found {compilers}")
    print(compilers[0], flush=True)
