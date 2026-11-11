"""Decompress and verify an Inno archive to a null sink, without installing it."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / ".tools/innounp/innounp.exe"
TOOL_SHA256 = "ab58400460cfda503ec88702dd3d82f6ee207b28e0548c9f8861ef008f85b644"


def run_tool(setup, command):
    result = subprocess.run(
        [str(TOOL), command, "-b", "-m", "-o", "-u", str(setup)],
        capture_output=True, timeout=3600,
    )
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    return result.returncode, output


def verify(setup, log):
    # Innounp can return zero after a per-file checksum failure, so also compare
    # every successfully checked file with its complete archive listing.
    listing_exit, listing = run_tool(setup, "-s")
    test_exit, output = run_tool(setup, "-t")
    expected = Counter(line[2:].strip() for line in listing.splitlines()
                       if line.startswith("  ") and line.strip())
    checked = Counter(re.findall(r"^#\d+ (.+?) - checked\s*$", output, re.MULTILINE))
    errors = [line for line in (listing + output).splitlines()
              if re.search(r"^\s*\*\*\*|^Error|files are corrupt|not supported", line, re.I)]
    valid = (listing_exit == 0 and test_exit == 0 and bool(expected)
             and expected == checked and not errors)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("ARCHIVE LIST\n" + listing + "\nINTEGRITY TEST\n" + output, encoding="utf-8")
    result = {
        "passed": valid, "archive": str(setup), "listing_exit": listing_exit,
        "test_exit": test_exit, "expected_files": sum(expected.values()),
        "checked_files": sum(checked.values()), "errors": errors,
        "missing_checks": list((expected - checked).elements()),
        "unexpected_checks": list((checked - expected).elements()),
        "tool_sha256": hashlib.sha256(TOOL.read_bytes()).hexdigest(),
        "log": str(log),
    }
    log.with_suffix(".json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("setup", type=Path)
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    if not TOOL.is_file():
        parser.error("Place the trusted portable Innounp 2.71.3 in .tools/innounp first.")
    if hashlib.sha256(TOOL.read_bytes()).hexdigest() != TOOL_SHA256:
        parser.error("Innounp does not match the pinned 2.71.3 binary.")
    result = verify(args.setup.resolve(strict=True), args.log.resolve())
    print(json.dumps(result, ensure_ascii=True))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
