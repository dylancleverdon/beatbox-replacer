#pragma once

#include "Types.h"

#include <vector>

namespace bbr
{

// Offline analysis of a whole mono recording: runs OnsetDetector over the buffer (in 512-sample
// blocks; results are block-size invariant) and extracts features for every onset with
// FeatureExtractor. This uses exactly the same code path as live mode, so a hit learned
// offline produces the same features as the same hit heard live.
// Onsets whose analysis window runs past the end of the buffer are still returned; the missing
// tail is treated as silence.
std::vector<DetectedHit> analyzeBuffer (const float* mono, int64_t numSamples, double sampleRate,
                                        const DetectorSettings& detector,
                                        const FeatureSettings& features);

// Copies the audio around an onset into a TrainingHit-style snippet:
// kSnippetPreMs before the onset to kSnippetPostMs after it, clipped to the buffer.
// onsetOffsetOut receives the index of the onset inside the returned snippet.
std::vector<float> makeSnippet (const float* mono, int64_t numSamples, int64_t onsetSample,
                                double sampleRate, int& onsetOffsetOut);

// Recomputes TrainingHit::features (and peakDb) from each hit's snippet for the given settings,
// using a FeatureExtractor prepared at each hit's own sample rate.
void recomputeFeatures (std::vector<TrainingHit>& hits, const FeatureSettings& settings);

// MIDI velocity for a hit.
//   dynamic == false -> fixedVelocity (clamped to 1..127)
//   dynamic == true  -> 100 + 4 * (peakDb - referenceDb), clamped to 1..127.
//                       referenceDb is the slot's median training peak (so a "typical" hit of
//                       that sound gets velocity 100); pass NaN to use -18 dBFS.
int velocityFor (float peakDb, float referenceDb, bool dynamic, int fixedVelocity) noexcept;

} // namespace bbr
