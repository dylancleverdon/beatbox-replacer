#pragma once

// Shared plain-data types for the JUCE-free analysis core.
// Everything in Source/core must compile with a plain C++17 compiler and must not include JUCE,
// so it can be unit-tested on any machine (see tests/CoreTests.cpp).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bbr
{

// ---------------------------------------------------------------------------------------------
// Slots ("sounds"): Kick, Snare, Hi-hat, ... Each slot has a stable id in [0, kMaxSlots).
// Ids are reused after a slot is deleted, so per-slot arrays can be indexed directly by id.
// ---------------------------------------------------------------------------------------------
constexpr int kMaxSlots = 8;

// Training label for hits that should produce no MIDI (breaths, clicks, mic bumps...).
constexpr int kIgnoreSlotId = -1;

// "No decision yet": a group or hit that has not been assigned to a slot.
// Hits with this label are never added to the training set.
constexpr int kUnassigned = -2;

struct SlotInfo
{
    int id = 0;          // [0, kMaxSlots)
    std::string name;    // "Kick"
    int note = 36;       // MIDI note number (Ableton names 36 "C1")
};

// ---------------------------------------------------------------------------------------------
// Analysis settings
// ---------------------------------------------------------------------------------------------
struct DetectorSettings
{
    float thresholdDb = -48.0f; // absolute gate: a hop's level must exceed this to start a hit
    float riseDb = 9.0f;        // level must jump this far above the recent minimum
    float minGapMs = 60.0f;     // minimum time between two onsets
};

struct FeatureSettings
{
    // Length of audio analysed after each onset. This is also the live-mode latency:
    // a live MIDI note is sent windowMs after the hit starts.
    float windowMs = 20.0f;
};

constexpr float kMinWindowMs = 8.0f;
constexpr float kMaxWindowMs = 60.0f;

// ---------------------------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------------------------
constexpr int kNumBands = 16;
constexpr int kNumFeatures = kNumBands + 6;

// Fixed-size so it can live on the audio thread without allocation. Layout:
//   [0, kNumBands)   log band energies on a mel-like scale (60 Hz .. min(16 kHz, 0.45 * sr)),
//                    with the mean across bands removed (spectral shape, loudness-independent)
//   [kNumBands + 0]  log2 of spectral centroid in Hz
//   [kNumBands + 1]  spectral flatness, in log domain (log(geo mean) - log(arith mean))
//   [kNumBands + 2]  zero-crossing rate, crossings per millisecond
//   [kNumBands + 3]  envelope shape: dB(energy of 2nd half of window) - dB(energy of 1st half)
//   [kNumBands + 4]  low ratio: dB(energy below 250 Hz) - dB(total energy)
//   [kNumBands + 5]  high ratio: dB(energy above 5 kHz) - dB(total energy)
using Features = std::array<float, kNumFeatures>;

enum FeatureIndex
{
    kFeatCentroid = kNumBands + 0,
    kFeatFlatness = kNumBands + 1,
    kFeatZcr = kNumBands + 2,
    kFeatEnvelope = kNumBands + 3,
    kFeatLowRatio = kNumBands + 4,
    kFeatHighRatio = kNumBands + 5,
};

// A hit found by analysis (offline or live).
struct DetectedHit
{
    int64_t onsetSample = 0;   // sample index of the onset in the analysed stream
    float peakDb = -120.0f;    // peak absolute level within the analysis window, dBFS
    Features features {};
};

// A training example: a labelled hit plus the audio around it, so features can be recomputed
// when the analysis window changes and so the hit can be auditioned later.
constexpr float kSnippetPreMs = 5.0f;    // audio kept before the onset
constexpr float kSnippetPostMs = 150.0f; // audio kept after the onset (>= kMaxWindowMs)

struct TrainingHit
{
    int slotId = kUnassigned;   // a slot id, or kIgnoreSlotId
    double sampleRate = 48000.0;
    int onsetOffset = 0;        // index of the onset inside snippet
    std::vector<float> snippet; // mono audio, (kSnippetPreMs + kSnippetPostMs) long when available
    float peakDb = -120.0f;
    Features features {};       // valid for the FeatureSettings last passed to recomputeFeatures()
};

// Ableton-style note name: 60 -> "C3", 36 -> "C1", 42 -> "F#1", 0 -> "C-2".
std::string noteName (int midiNote);

// Inverse of noteName(); accepts "C1", "c#1", "Db1", "F#-1". Returns -1 if it can't parse or
// the result is outside 0..127.
int parseNoteName (const std::string& text);

} // namespace bbr
