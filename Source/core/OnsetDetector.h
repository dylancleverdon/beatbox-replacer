#pragma once

#include "Types.h"

#include <vector>

namespace bbr
{

// Streaming onset detector for percussive vocal sounds.
//
// Algorithm (all times converted with the sample rate, so behaviour is rate-independent):
//   * hop = max(16, round(sampleRate * 0.0025)) samples (2.5 ms).
//   * For every completed hop compute two levels in dB (10*log10(mean square + 1e-12)):
//       - fullDb: the raw signal
//       - hfDb:   the signal through a 2nd-order high-pass at 4 kHz (state kept across calls)
//     so quiet hi-hats ("ts") trigger as reliably as loud kicks ("b").
//   * Keep the last round(30 ms / hop) hop levels of each. rise = level - min(history).
//   * Trigger when the min-gap has elapsed since the last onset AND either
//       (fullDb >= thresholdDb       && riseFull >= riseDb) or
//       (hfDb   >= thresholdDb - 6   && riseHf   >= riseDb).
//   * Onset refinement: look at the triggering hop and the hop before it; the onset is the
//     first sample in that span whose |x| >= 0.5 * max|x| over the span.
//     (Reported onsets can therefore be up to 2 hops in the past relative to the current sample.)
//   * After an onset, the next onset is not allowed until minGapMs later.
//
// Results MUST be identical regardless of how the input is split into blocks
// (tests feed the same signal in blocks of 1, 64, 512 and the whole buffer).
class OnsetDetector
{
public:
    // Allocates. Also calls reset().
    void prepare (double sampleRate);

    // Clears all state; the sample clock restarts at 0. Real-time safe.
    void reset() noexcept;

    // Real-time safe (plain copy).
    void setSettings (const DetectorSettings& s) noexcept { settings = s; }
    const DetectorSettings& getSettings() const noexcept { return settings; }

    // Feeds n mono samples. Writes the absolute sample index (counted from reset()) of each
    // onset found into onsetsOut, up to maxOnsets, and returns how many were written.
    // Onsets are reported in increasing order. Real-time safe: no allocation, no locks.
    int process (const float* x, int n, int64_t* onsetsOut, int maxOnsets) noexcept;

    // Total samples consumed since reset().
    int64_t getSamplesProcessed() const noexcept { return samplesProcessed; }

    int getHopSize() const noexcept { return hop; }

private:
    double sampleRate = 48000.0;
    DetectorSettings settings;
    int hop = 120;
    int historyLen = 12;
    int64_t samplesProcessed = 0;

    // implementation state (sized in prepare())
    std::vector<float> hopBuffer;   // last 2 hops of raw samples, ring
    std::vector<float> fullHistory; // ring of hop levels
    std::vector<float> hfHistory;
    int historyPos = 0;
    int historyCount = 0;
    int hopFill = 0;
    double fullAcc = 0.0, hfAcc = 0.0;
    int64_t lastOnset = -1;
    // high-pass biquad state + coefficients
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
};

} // namespace bbr
