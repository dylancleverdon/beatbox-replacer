#include "FeatureExtractor.h"

#include <algorithm>
#include <cmath>

namespace bbr
{

namespace
{
    constexpr int kMinFftSize = 256;
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    constexpr double kLowestBandHz = 60.0;
    constexpr double kLowRatioHz = 250.0;
    constexpr double kHighRatioHz = 5000.0;

    double hzToMel (double hz) noexcept { return 2595.0 * std::log10 (1.0 + hz / 700.0); }
    double melToHz (double mel) noexcept { return 700.0 * (std::pow (10.0, mel / 2595.0) - 1.0); }

    double powerDb (double energy) noexcept { return 10.0 * std::log10 (energy + 1e-10); }
}

void FeatureExtractor::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    FeatureSettings longest;
    longest.windowMs = kMaxWindowMs;
    const int maxSize = std::max (kMinFftSize, Fft::nextPowerOfTwo (windowSamples (longest)));

    ffts.clear();

    for (int size = kMinFftSize; size <= maxSize; size *= 2)
    {
        ffts.emplace_back();
        ffts.back().prepare (size);
    }

    frame.assign ((size_t) maxSize, 0.0f);
    magnitudes.assign ((size_t) (maxSize / 2 + 1), 0.0f);
    scratch.assign ((size_t) maxSize, std::complex<float>());
}

int FeatureExtractor::windowSamples (const FeatureSettings& s) const noexcept
{
    float ms = s.windowMs;

    if (! (ms >= kMinWindowMs)) // also catches NaN
        ms = kMinWindowMs;

    if (ms > kMaxWindowMs)
        ms = kMaxWindowMs;

    return std::max (1, (int) std::lround ((double) ms * 0.001 * sampleRate));
}

// Everything below depends on the settings only through windowSamples(s): LiveEngine relies on
// this to reproduce analyzeBuffer() features exactly.
void FeatureExtractor::extract (const float* x, int numSamples, const FeatureSettings& s,
                                Features& featuresOut, float& peakDbOut) noexcept
{
    featuresOut.fill (0.0f);
    peakDbOut = -120.0f;

    if (ffts.empty())
        return;

    const int requested = windowSamples (s);
    const Fft* fft = &ffts.back();

    for (const Fft& candidate : ffts)
    {
        if (candidate.getSize() >= requested)
        {
            fft = &candidate;
            break;
        }
    }

    const int fftSize = fft->getSize();
    const int window = std::min (requested, fftSize);
    const int available = x == nullptr ? 0 : std::clamp (numSamples, 0, window);
    float* const buf = frame.data();

    std::copy (x, x + available, buf);
    std::fill (buf + available, buf + fftSize, 0.0f);

    // ---- time domain (un-windowed) ----
    float peak = 0.0f;
    int crossings = 0;

    for (int i = 0; i < window; ++i)
    {
        peak = std::max (peak, std::abs (buf[i]));

        if (i > 0 && ((buf[i - 1] < 0.0f) != (buf[i] < 0.0f)))
            ++crossings;
    }

    const int half = window / 2;
    double firstHalf = 0.0, secondHalf = 0.0;

    for (int i = 0; i < half; ++i)
        firstHalf += (double) buf[i] * (double) buf[i];

    for (int i = half; i < 2 * half; ++i)
        secondHalf += (double) buf[i] * (double) buf[i];

    const double windowMs = (double) window * 1000.0 / sampleRate;

    peakDbOut = (float) (20.0 * std::log10 ((double) peak + 1e-9));
    featuresOut[kFeatZcr] = (float) ((double) crossings / windowMs);
    featuresOut[kFeatEnvelope] = (float) (powerDb (secondHalf) - powerDb (firstHalf));

    // ---- Hann window (cosine by rotation: cheap and deterministic) ----
    {
        const double step = kTwoPi / (double) window;
        const double cosStep = std::cos (step), sinStep = std::sin (step);
        double c = std::cos (0.5 * step), sn = std::sin (0.5 * step);

        for (int i = 0; i < window; ++i)
        {
            buf[i] = (float) ((double) buf[i] * (0.5 - 0.5 * c));
            const double nextC = c * cosStep - sn * sinStep;
            sn = sn * cosStep + c * sinStep;
            c = nextC;
        }
    }

    float* const mags = magnitudes.data();
    fft->performRealMagnitudes (buf, mags, scratch.data());

    const int numBins = fftSize / 2;
    const double binHz = sampleRate / (double) fftSize;
    const double topHz = std::min (16000.0, 0.45 * sampleRate);

    // ---- whole-spectrum features over bins 1 .. N/2 ----
    double total = 0.0, weighted = 0.0, low = 0.0, high = 0.0;
    double flatLogSum = 0.0, flatSum = 0.0;
    int flatCount = 0;

    for (int k = 1; k <= numBins; ++k)
    {
        const double power = (double) mags[k] * (double) mags[k];
        const double hz = (double) k * binHz;

        total += power;
        weighted += hz * power;

        if (hz < kLowRatioHz)
            low += power;

        if (hz > kHighRatioHz)
            high += power;

    }

    // Energies are floored 60 dB below the window's total energy instead of at an absolute
    // level, so every feature stays exactly loudness-independent (a quiet kick must look like a
    // loud one) and near-empty bands can't dominate the spectral shape.
    const double floorEnergy = total * 1.0e-6 + 1.0e-30;

    for (int k = 1; k <= numBins; ++k)
    {
        const double hz = (double) k * binHz;

        if (hz >= kLowestBandHz && hz <= topHz)
        {
            const double power = (double) mags[k] * (double) mags[k] + floorEnergy;
            flatLogSum += std::log (power);
            flatSum += power;
            ++flatCount;
        }
    }

    const double centroid = total > 0.0 ? weighted / total : 0.0;
    featuresOut[kFeatCentroid] = (float) std::log2 (std::max (20.0, centroid));

    if (flatCount > 0)
        featuresOut[kFeatFlatness] = (float) (flatLogSum / flatCount - std::log (flatSum / flatCount));

    const double totalDb = powerDb (total + floorEnergy);
    featuresOut[kFeatLowRatio] = (float) (powerDb (low + floorEnergy) - totalDb);
    featuresOut[kFeatHighRatio] = (float) (powerDb (high + floorEnergy) - totalDb);

    // ---- mel bands: edges[b] .. edges[b + 1] - 1 are band b's bins ----
    int edges[kNumBands + 1];
    const double melLow = hzToMel (kLowestBandHz);
    const double melHigh = hzToMel (topHz);

    for (int b = 0; b <= kNumBands; ++b)
    {
        const double hz = melToHz (melLow + (melHigh - melLow) * (double) b / (double) kNumBands);
        int bin = (int) std::ceil (hz / binHz - 1e-9);
        bin = std::clamp (bin, 1, numBins + 1);

        if (b > 0)
            bin = std::max (bin, edges[b - 1] + 1); // every band gets at least one bin

        edges[b] = bin;
    }

    if (edges[kNumBands] > numBins + 1)
    {
        edges[kNumBands] = numBins + 1;

        for (int b = kNumBands - 1; b >= 0; --b)
            edges[b] = std::min (edges[b], edges[b + 1] - 1);
    }

    double bandDb[kNumBands];
    double meanDb = 0.0;

    for (int b = 0; b < kNumBands; ++b)
    {
        double energy = 0.0;

        for (int k = edges[b]; k < edges[b + 1]; ++k)
            energy += (double) mags[k] * (double) mags[k];

        bandDb[b] = powerDb (energy + floorEnergy);
        meanDb += bandDb[b];
    }

    meanDb /= (double) kNumBands;

    for (int b = 0; b < kNumBands; ++b)
        featuresOut[(size_t) b] = (float) (bandDb[b] - meanDb);
}

} // namespace bbr
