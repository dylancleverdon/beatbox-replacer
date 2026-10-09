#pragma once

#include "Fft.h"
#include "Types.h"

#include <complex>
#include <vector>

namespace bbr
{

// Computes the Features vector (see Types.h for the exact layout) for the audio window that
// starts at an onset. Feature definitions use Hz and milliseconds only, so features computed
// at 44.1 kHz and 48 kHz are directly comparable.
//
// Method: take windowSamples(settings) samples starting at the onset, apply a Hann window,
// zero-pad to the next power of two (minimum 256) and take magnitude spectrum. Band energies
// are summed |X|^2 over kNumBands rectangular mel-spaced bands between 60 Hz and
// min(16 kHz, 0.45 * sampleRate); every band gets at least one FFT bin. Spectral energies are
// floored at 1e-6 * the window's total energy (60 dB down) before taking logs, so all features
// are independent of the hit's loudness. Time-domain features (zero-crossing rate, envelope
// shape, peak) use the un-windowed samples.
class FeatureExtractor
{
public:
    // Allocates scratch buffers big enough for kMaxWindowMs at this sample rate.
    void prepare (double sampleRate);

    double getSampleRate() const noexcept { return sampleRate; }

    // Number of samples the analysis window spans for these settings (clamped to
    // [kMinWindowMs, kMaxWindowMs]).
    int windowSamples (const FeatureSettings& s) const noexcept;

    // x points at the onset sample; numSamples must be >= windowSamples(s) (extra samples are
    // ignored). If numSamples is smaller, the missing tail is treated as silence.
    // Real-time safe (uses only the scratch buffers allocated by prepare()).
    void extract (const float* x, int numSamples, const FeatureSettings& s,
                  Features& featuresOut, float& peakDbOut) noexcept;

private:
    double sampleRate = 48000.0;
    std::vector<Fft> ffts;                    // one per power-of-two size 256 .. max, built in prepare()
    std::vector<float> frame;                 // windowed, zero-padded input
    std::vector<float> magnitudes;
    std::vector<std::complex<float>> scratch;
};

} // namespace bbr
