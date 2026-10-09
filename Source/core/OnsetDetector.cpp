#include "OnsetDetector.h"

#include <algorithm>
#include <cmath>

namespace bbr
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    float levelDb (double sumOfSquares, int numSamples) noexcept
    {
        return (float) (10.0 * std::log10 (sumOfSquares / (double) numSamples + 1e-12));
    }

    float minOf (const std::vector<float>& values, int count) noexcept
    {
        float result = values[0];

        for (int i = 1; i < count; ++i)
            result = std::min (result, values[(size_t) i]);

        return result;
    }

    float flushTiny (float v) noexcept
    {
        return std::abs (v) < 1.0e-20f ? 0.0f : v;
    }
}

void OnsetDetector::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    hop = std::max (16, (int) std::lround (sampleRate * 0.0025));
    historyLen = std::max (1, (int) std::lround (0.030 * sampleRate / (double) hop));

    hopBuffer.assign ((size_t) (2 * hop), 0.0f);
    hfHopBuffer.assign ((size_t) (2 * hop), 0.0f);
    fullHistory.assign ((size_t) historyLen, 0.0f);
    hfHistory.assign ((size_t) historyLen, 0.0f);

    // RBJ cookbook 2nd-order high-pass, Q = 1/sqrt(2) (Butterworth).
    const double cutoff = std::min (4000.0, 0.45 * sampleRate);
    const double w0 = 2.0 * kPi * cutoff / sampleRate;
    const double cosW0 = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * 0.70710678118654752);
    const double a0 = 1.0 + alpha;

    b0 = (float) ((1.0 + cosW0) * 0.5 / a0);
    b1 = (float) (-(1.0 + cosW0) / a0);
    b2 = b0;
    a1 = (float) (-2.0 * cosW0 / a0);
    a2 = (float) ((1.0 - alpha) / a0);

    reset();
}

void OnsetDetector::reset() noexcept
{
    samplesProcessed = 0;
    std::fill (hopBuffer.begin(), hopBuffer.end(), 0.0f);
    std::fill (hfHopBuffer.begin(), hfHopBuffer.end(), 0.0f);
    std::fill (fullHistory.begin(), fullHistory.end(), 0.0f);
    std::fill (hfHistory.begin(), hfHistory.end(), 0.0f);
    historyPos = 0;
    historyCount = 0;
    hopFill = 0;
    fullAcc = 0.0;
    hfAcc = 0.0;
    prevFullAcc = 0.0;
    prevHfAcc = 0.0;
    lastOnset = -1;
    z1 = 0.0f;
    z2 = 0.0f;
}

int OnsetDetector::process (const float* x, int n, int64_t* onsetsOut, int maxOnsets) noexcept
{
    if (x == nullptr || n <= 0)
        return 0;

    if (hopBuffer.empty()) // prepare() not called
    {
        samplesProcessed += n;
        return 0;
    }

    // The hop buffers hold the previous hop in [0, hop) and the hop being filled in [hop, 2 * hop).
    float* const previous = hopBuffer.data();
    float* const current = previous + hop;
    float* const hfPrevious = hfHopBuffer.data();
    float* const hfCurrent = hfPrevious + hop;
    int numFound = 0;
    int pos = 0;

    while (pos < n)
    {
        // Every sample goes through the same per-sample arithmetic in the same order whatever the
        // block size, so results are bit-identical for any split of the input.
        const int count = std::min (n - pos, hop - hopFill);
        double full = fullAcc;
        double hf = hfAcc;
        float s1 = z1;
        float s2 = z2;

        for (int i = 0; i < count; ++i)
        {
            const float in = x[pos + i];
            current[hopFill + i] = in;

            const float out = b0 * in + s1;
            s1 = b1 * in - a1 * out + s2;
            s2 = b2 * in - a2 * out;
            hfCurrent[hopFill + i] = out;

            full += (double) in * (double) in;
            hf += (double) out * (double) out;
        }

        fullAcc = full;
        hfAcc = hf;
        z1 = s1;
        z2 = s2;
        hopFill += count;
        pos += count;
        samplesProcessed += count;

        if (hopFill < hop)
            break;

        // A hop is complete. Levels span this hop and the previous one.
        const float fullDb = levelDb (prevFullAcc + fullAcc, 2 * hop);
        const float hfDb = levelDb (prevHfAcc + hfAcc, 2 * hop);

        if (historyCount > 0)
        {
            const float riseFull = fullDb - minOf (fullHistory, historyCount);
            const float riseHf = hfDb - minOf (hfHistory, historyCount);

            const bool fullTrigger = fullDb >= settings.thresholdDb && riseFull >= settings.riseDb;
            const bool hfTrigger = hfDb >= settings.thresholdDb - 6.0f && riseHf >= settings.riseDb;

            if (fullTrigger || hfTrigger)
            {
                // Refine: first sample of (previous hop + this hop) reaching half the span's peak.
                const float* const span = fullTrigger ? previous : hfPrevious;
                const int64_t spanStart = samplesProcessed - 2 * hop;
                const int first = spanStart < 0 ? (int) -spanStart : 0;
                float peak = 0.0f;

                for (int i = first; i < 2 * hop; ++i)
                    peak = std::max (peak, std::abs (span[i]));

                const float target = 0.5f * peak;
                int index = first;

                for (int i = first; i < 2 * hop; ++i)
                {
                    if (std::abs (span[i]) >= target)
                    {
                        index = i;
                        break;
                    }
                }

                const int64_t onset = spanStart + index;
                const double gapMs = std::max (0.0, (double) settings.minGapMs);
                const int64_t minGap = (int64_t) std::llround (gapMs * 0.001 * sampleRate);

                if (onset > lastOnset && (lastOnset < 0 || onset - lastOnset >= minGap))
                {
                    lastOnset = onset;

                    if (onsetsOut != nullptr && numFound < maxOnsets)
                        onsetsOut[numFound++] = onset;
                }
            }
        }

        fullHistory[(size_t) historyPos] = fullDb;
        hfHistory[(size_t) historyPos] = hfDb;
        historyPos = (historyPos + 1) % historyLen;
        historyCount = std::min (historyCount + 1, historyLen);

        std::copy (current, current + hop, previous);
        std::copy (hfCurrent, hfCurrent + hop, hfPrevious);
        hopFill = 0;
        prevFullAcc = fullAcc;
        prevHfAcc = hfAcc;
        fullAcc = 0.0;
        hfAcc = 0.0;

        // Avoid denormals in long silences (done at hop boundaries, so still block-size invariant).
        z1 = flushTiny (z1);
        z2 = flushTiny (z2);
    }

    return numFound;
}

} // namespace bbr
