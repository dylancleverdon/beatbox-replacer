# Beatbox Replacer

A plug-in for Ableton Live that turns your beatboxing into MIDI drums. You teach it your own
sounds (kick, snare, hi-hat and up to 8 in total), then it listens to your beatbox track through
its sidechain input and plays each hit as a MIDI note: live while you record, or as a MIDI clip you
drag straight into your arrangement.

## Download

These links always point to the newest version:

- **Windows:** [BeatboxReplacer-Setup.exe](https://github.com/dylancleverdon/beatbox-replacer/releases/latest/download/BeatboxReplacer-Setup.exe)
- **Mac:** [BeatboxReplacer-macOS.pkg](https://github.com/dylancleverdon/beatbox-replacer/releases/latest/download/BeatboxReplacer-macOS.pkg)

Everything the plug-in needs is in that one installer.

**Windows:** run the installer. Windows may say "Windows protected your PC" because the installer
isn't code-signed. Click **More info → Run anyway**. The plug-in goes into
`C:\Program Files\Common Files\VST3`, where Ableton finds it.

**Mac:** double-click the .pkg. If macOS blocks it, open **System Settings → Privacy & Security**,
scroll down and click **Open Anyway**, then run it again. In Ableton use the **VST3** version.
Live can't receive MIDI from Audio Unit plug-ins.

Then open Ableton. If the plug-in doesn't appear, go to **Preferences → Plug-ins**, make sure
**Use VST3 Plug-in System Folders** is on, and click **Rescan**.

## Quick start

1. Make 3 tracks: your **beatbox audio** track, a **MIDI track with Beatbox Replacer** (sidechain
   set to the beatbox track), and a **MIDI track with a Drum Rack** whose *MIDI From* is the
   Beatbox Replacer track.
2. **Learn tab:** press **Learn**, play a section with each of your sounds a few times, stop.
3. Pick which sound each group of hits is, then press **Add to training**.
4. **Play tab:** record on the Drum Rack track for live MIDI, or arm **Capture**, play the
   section and drag the clip into Ableton.

The full walkthrough, settings and troubleshooting are in **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)**.

## Updates

When you open the plug-in it checks for a newer version. If there is one, click **Install update**
in the banner at the top, then restart Ableton. If the in-place update ever fails, the banner
offers **Run installer**, which downloads and opens the full installer.

New versions are published automatically: every push to this repository's default branch is
built for Windows and Mac by GitHub Actions and released as `v1.0.<build number>`.

## For developers

Requirements: CMake 3.22+, a C++17 compiler (Visual Studio 2022+, Xcode 15+, or GCC/Clang on
Linux). JUCE 8 is downloaded automatically.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

- The built plug-in is in `build/BeatboxReplacer_artefacts/Release/VST3/`.
- Building offline: add `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.
- Analysis core only, no JUCE: `-DBBR_BUILD_PLUGIN=OFF`.
- To copy the plug-in into your system plug-in folder after each build, add `-DBBR_COPY_AFTER_BUILD=ON`.
- `scripts/check-compile.sh <file.cpp>` compile-checks one file against a configured
  `build-linux` folder.

How it fits together, including the real-time rules and the release and update format, is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Releasing:** push to the default branch. To publish from another branch, run the *Build*
workflow by hand in the Actions tab and tick **publish**.

JUCE 8 is used under its free licence terms for personal, non-commercial use.
