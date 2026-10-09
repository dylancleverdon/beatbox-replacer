# Beatbox Replacer – architecture

## What it does

An instrument plugin (VST3 on Windows/macOS, plus AU on macOS) for Ableton Live. Beatbox audio
reaches it through the plugin's **sidechain** input. It finds every hit, works out which of your
sounds (slots: Kick, Snare, Hi-hat, ... up to 8) the hit is, and outputs a MIDI note for that slot.

Two ways to get MIDI:

1. **Live** – notes are sent from the plugin's MIDI output as you beatbox / as the beatbox track
   plays. Record them on a third MIDI track whose *MIDI From* is the plugin's track. Each note
   comes out `window` ms (default 20 ms) after the hit starts, because the plugin has to hear
   that much of the sound before it can tell what it is.
2. **Capture → drag** – arm Capture, play the beatbox section, stop. The plugin keeps the exact
   onset positions on the song timeline (no lateness) and gives you a MIDI clip to drag into
   Ableton (or save as a .mid).

Teaching it your sounds (**Learn**): press Learn, play a section with your sounds, stop. The plugin
finds each hit, groups similar hits automatically, and you assign each group to a slot (or to
"Ignore"). Clicking a single hit plays it and lets you move it to another slot. "Add to training"
stores the labelled hits (with ~155 ms of audio each) in the project and in exported profiles.

## Layout

```
Source/core/     JUCE-free C++17 analysis core (unit-tested on Linux in CI)
  Types.h          shared types, constants, note names
  Fft              radix-2 FFT
  OnsetDetector    streaming onset detection (block-size invariant)
  FeatureExtractor 22-dim spectral/temporal features per hit
  Analysis         offline analyzeBuffer(), snippets, velocity
  Classifier       kNN model (immutable, audio-thread safe)
  Clustering       k-means++, auto-k by silhouette, slot suggestions
  LearnSession     one Learn pass: audio, hits, groups, assignments
  LiveEngine       real-time detect + classify for processBlock
  Capture          capture takes -> MIDI notes
  MidiFile         Standard MIDI File writer/parser
  Version          semantic version compare (updater)
Source/plugin/   JUCE plugin
  PluginProcessor  audio thread, state, learn/capture recording, MIDI out
  ProfileIO        slots + training <-> XML
  PluginEditor     tabbed UI (Learn / Play / Setup) + ui/ components
  Updater          GitHub release check + in-place install
tests/           core unit tests with synthetic beatbox-like signals
installer/       Inno Setup script (Windows), pkg scripts (macOS)
.github/workflows/build.yml   build, test, package, release
```

## Real-time rules

`processBlock` never allocates, never blocks on a lock, never does I/O. Data shared with the
message thread uses atomics, preallocated buffers, or `juce::SpinLock` with `ScopedTryLock` on
the audio side. The classifier model is an immutable `std::shared_ptr<const ClassifierModel>`;
the message thread swaps in a new one and keeps the old one alive for a few seconds so the audio
thread never releases the last reference.

## Releases and updates

Every push to the repository's default branch builds Windows + macOS in GitHub Actions and
publishes release `v1.0.<run number>` with:

| asset | used for |
|---|---|
| `BeatboxReplacer-Setup.exe` | first install on Windows (and fallback) |
| `BeatboxReplacer-Windows.zip` | in-plugin update on Windows (the `.vst3` bundle) |
| `BeatboxReplacer-macOS.pkg` | first install on macOS (and fallback) |
| `BeatboxReplacer-macOS.zip` | in-plugin update on macOS (`.vst3` + `.component`) |

The plugin checks `GET /repos/<owner>/<repo>/releases/latest` when its window opens and, if the
tag is newer than its own version, offers **Install update**: it downloads the zip and swaps the
files inside the installed bundle (installers grant the user write access to the bundle folder),
then asks you to restart the DAW.
