#!/usr/bin/env bash
# Compile-checks one plugin source file using the flags CMake generated, without running the
# build system (safe to run several at once). Needs a configured build dir:
#   cmake -S . -B build-linux -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
#         -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
#   ninja -C build-linux BeatboxReplacer_artefacts/JuceLibraryCode/JuceHeader.h
# Usage: scripts/check-compile.sh Source/plugin/PluginProcessor.cpp [build-dir]
set -euo pipefail
src="$(realpath "$1")"
build="${2:-build-linux}"
python3 - "$src" "$build" <<'PY'
import json, shlex, subprocess, sys, os, tempfile
src, build = sys.argv[1], sys.argv[2]
cmds = json.load(open(os.path.join(build, "compile_commands.json")))
entry = next((c for c in cmds if os.path.realpath(c["file"]) == src), None)
if entry is None:
    sys.exit(f"{src} is not in {build}/compile_commands.json (is it listed in CMakeLists.txt?)")
args = shlex.split(entry["command"])
out = tempfile.mktemp(suffix=".o")
if "-o" in args:
    args[args.index("-o") + 1] = out
# drop dependency-file flags so nothing is written into the build dir
clean, skip = [], False
for a in args:
    if skip: skip = False; continue
    if a in ("-MF", "-MT", "-MQ"): skip = True; continue
    if a in ("-MD", "-MMD"): continue
    clean.append(a)
r = subprocess.run(clean, cwd=entry["directory"])
if os.path.exists(out): os.remove(out)
sys.exit(r.returncode)
PY
