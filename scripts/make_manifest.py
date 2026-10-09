#!/usr/bin/env python3
"""Release helper for .github/workflows/build.yml.

  make_manifest.py check-zip {windows|macos} <zip>
      Checks that an update zip has the layout the in-plugin updater expects
      (see Source/plugin/Updater.h). Exits non-zero with a message if not.

  make_manifest.py write --version 1.0.42 --assets <dir> [--notes-file F | --notes TEXT] --out latest.json
      Checks the four release assets in <dir> and writes latest.json:
        { "version": "1.0.42", "tag": "v1.0.42", "notes": "...",
          "windows": { "zip": "BeatboxReplacer-Windows.zip", "sha256": "<hex>", "installer": "BeatboxReplacer-Setup.exe" },
          "macos":   { "zip": "BeatboxReplacer-macOS.zip",   "sha256": "<hex>", "installer": "BeatboxReplacer-macOS.pkg" } }
"""

import argparse
import hashlib
import json
import os
import re
import sys
import zipfile

ASSETS = {
    "windows": {"zip": "BeatboxReplacer-Windows.zip", "installer": "BeatboxReplacer-Setup.exe"},
    "macos": {"zip": "BeatboxReplacer-macOS.zip", "installer": "BeatboxReplacer-macOS.pkg"},
}

# Top-level folders allowed in each update zip ("__MACOSX" holds ditto's sequestered xattrs).
ALLOWED_ROOTS = {
    "windows": {"BeatboxReplacer.vst3"},
    "macos": {"BeatboxReplacer.vst3", "BeatboxReplacer.component", "__MACOSX"},
}

# Files that must be present (and non-empty).
REQUIRED_FILES = {
    "windows": [
        "BeatboxReplacer.vst3/Contents/x86_64-win/BeatboxReplacer.vst3",
    ],
    "macos": [
        "BeatboxReplacer.vst3/Contents/Info.plist",
        "BeatboxReplacer.vst3/Contents/MacOS/BeatboxReplacer",
        "BeatboxReplacer.vst3/Contents/_CodeSignature/CodeResources",
        "BeatboxReplacer.component/Contents/Info.plist",
        "BeatboxReplacer.component/Contents/MacOS/BeatboxReplacer",
        "BeatboxReplacer.component/Contents/_CodeSignature/CodeResources",
    ],
}

# Mach-O binaries that must keep their executable bit (ditto stores unix modes).
EXECUTABLES = {
    "windows": [],
    "macos": [
        "BeatboxReplacer.vst3/Contents/MacOS/BeatboxReplacer",
        "BeatboxReplacer.component/Contents/MacOS/BeatboxReplacer",
    ],
}

ZIP_CREATE_SYSTEM_UNIX = 3


class CheckError(Exception):
    pass


def check_zip(platform, path):
    """Validates an update zip; returns the list of entry names."""
    if not os.path.isfile(path):
        raise CheckError(f"{path}: not found")

    try:
        with zipfile.ZipFile(path) as z:
            infos = z.infolist()
            bad = z.testzip()
    except zipfile.BadZipFile as e:
        raise CheckError(f"{path}: not a valid zip ({e})")

    if bad is not None:
        raise CheckError(f"{path}: CRC error in {bad}")

    if not infos:
        raise CheckError(f"{path}: empty zip")

    by_name = {}
    for info in infos:
        name = info.filename
        if "\\" in name:
            raise CheckError(f"{path}: entry uses backslashes: {name!r}")
        parts = name.rstrip("/").split("/")
        if name.startswith("/") or ".." in parts or (parts and parts[0].endswith(":")):
            raise CheckError(f"{path}: unsafe entry path: {name!r}")
        root = parts[0]
        if root not in ALLOWED_ROOTS[platform]:
            allowed = ", ".join(sorted(ALLOWED_ROOTS[platform]))
            raise CheckError(f"{path}: unexpected top-level item {root!r} (allowed: {allowed})")
        by_name[name] = info

    for required in REQUIRED_FILES[platform]:
        info = by_name.get(required)
        if info is None:
            raise CheckError(f"{path}: missing {required}")
        if info.file_size == 0:
            raise CheckError(f"{path}: {required} is empty")

    for exe in EXECUTABLES[platform]:
        info = by_name[exe]
        mode = (info.external_attr >> 16) & 0o7777
        if info.create_system != ZIP_CREATE_SYSTEM_UNIX:
            print(f"warning: {path}: {exe} has no unix mode (made on system {info.create_system})",
                  file=sys.stderr)
        elif not mode & 0o111:
            raise CheckError(f"{path}: {exe} is not executable (mode {mode:o})")

    return [i.filename for i in infos]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_check_zip(args):
    names = check_zip(args.platform, args.zip)
    files = [n for n in names if not n.endswith("/")]
    print(f"{args.zip}: OK ({len(files)} files)")
    for n in names:
        print("  " + n)
    return 0


def cmd_write(args):
    version = args.version.strip()
    if version.startswith(("v", "V")):
        version = version[1:]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise CheckError(f"version must look like 1.0.42, got {args.version!r}")

    if args.notes_file is not None:
        with open(args.notes_file, encoding="utf-8", errors="replace") as f:
            notes = f.read()
    else:
        notes = args.notes or ""
    notes = notes.replace("\r\n", "\n").strip()
    if not notes:
        notes = f"Beatbox Replacer {version}"

    manifest = {"version": version, "tag": "v" + version, "notes": notes}

    for platform, assets in ASSETS.items():
        zip_path = os.path.join(args.assets, assets["zip"])
        installer_path = os.path.join(args.assets, assets["installer"])
        check_zip(platform, zip_path)
        if not os.path.isfile(installer_path) or os.path.getsize(installer_path) == 0:
            raise CheckError(f"{installer_path}: missing or empty")
        manifest[platform] = {
            "zip": assets["zip"],
            "sha256": sha256_of(zip_path),
            "installer": assets["installer"],
        }

    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    print(text, end="")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Beatbox Replacer release manifest tool")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("check-zip", help="validate an update zip's layout")
    p.add_argument("platform", choices=sorted(ASSETS))
    p.add_argument("zip")
    p.set_defaults(func=cmd_check_zip)

    p = sub.add_parser("write", help="write latest.json")
    p.add_argument("--version", required=True, help="e.g. 1.0.42")
    p.add_argument("--assets", required=True, help="folder holding the four release assets")
    notes = p.add_mutually_exclusive_group()
    notes.add_argument("--notes-file", help="UTF-8 text file with the release notes")
    notes.add_argument("--notes", help="release notes text")
    p.add_argument("--out", required=True, help="path of latest.json to write")
    p.set_defaults(func=cmd_write)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except CheckError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
