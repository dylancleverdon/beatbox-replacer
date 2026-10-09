#include "LiveEngine.h"

#include <algorithm>
#include <cmath>

namespace bbr
{

namespace
{
    constexpr int kPendingCapacity = 64;
    constexpr int kMaxSupportedBlock = 1 << 22;
}

void LiveEngine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxBlock = std::clamp (maxBlockSize, 1, kMaxSupportedBlock);

    detector.prepare (sampleRate);
    extractor.prepare (sampleRate);

    FeatureSettings longest;
    longest.windowMs = kMaxWindowMs;
    const int maxWindow = extractor.windowSamples (longest);
    const int hop = detector.getHopSize();
    const int ringSize = Fft::nextPowerOfTwo (maxWindow + 4 * hop + maxBlock + 64);

    ring.assign ((size_t) ringSize, 0.0f);
    ringMask = ringSize - 1;

    pending.assign ((size_t) kPendingCapacity, Pending { 0, 0 });
    onsetScratch.assign ((size_t) (maxBlock / hop + 2), 0); // at most one onset per completed hop
    windowScratch.assign ((size_t) maxWindow, 0.0f);

    reset();
}

void LiveEngine::reset() noexcept
{
    detector.reset();
    std::fill (ring.begin(), ring.end(), 0.0f);
    clock = 0;
    pendingCount = 0;
}

int LiveEngine::process (const float* mono, int n, const ClassifierModel* model, LiveHit* out, int maxOut) noexcept
{
    if (mono == nullptr || n <= 0 || ring.empty() || pending.empty())
        return 0;

    if (out == nullptr)
        maxOut = 0;

    const int64_t blockStart = clock;
    const int64_t ringSize = (int64_t) ring.size();
    const int capacity = (int) pending.size();
    const bool canClassify = model != nullptr && ! model->empty();
    const int newWindow = extractor.windowSamples (model != nullptr ? model->getFeatureSettings()
                                                                    : fallbackFeatures);
    int numOut = 0;

    for (int done = 0; done < n;)
    {
        const int len = std::min (maxBlock, n - done);
        const float* const chunk = mono + done;

        for (int i = 0; i < len; ++i)
            ring[(size_t) ((clock + i) & ringMask)] = chunk[i];

        const int found = detector.process (chunk, len, onsetScratch.data(), (int) onsetScratch.size());
        clock += len;
        done += len;

        for (int i = 0; i < found; ++i)
        {
            if (pendingCount == capacity) // drop the oldest
            {
                std::copy (pending.begin() + 1, pending.begin() + pendingCount, pending.begin());
                --pendingCount;
            }

            pending[(size_t) pendingCount++] = Pending { onsetScratch[(size_t) i], newWindow };
        }

        int kept = 0;

        for (int i = 0; i < pendingCount; ++i)
        {
            const Pending p = pending[(size_t) i];
            const int64_t windowEnd = p.onset + p.windowSamples;

            if (windowEnd > clock || (numOut >= maxOut && clock - p.onset <= ringSize))
            {
                // Not complete yet, or no room in `out`: keep it (a full `out` delays it a block).
                pending[(size_t) kept++] = p;
                continue;
            }

            if (clock - p.onset > ringSize) // audio already overwritten
                continue;

            const int window = std::min (p.windowSamples, (int) windowScratch.size());

            for (int j = 0; j < window; ++j)
                windowScratch[(size_t) j] = ring[(size_t) ((p.onset + j) & ringMask)];

            // extract() only depends on windowSamples(settings); this round-trips to `window`.
            FeatureSettings settings;
            settings.windowMs = (float) ((double) window * 1000.0 / sampleRate);

            LiveHit& hit = out[numOut++];
            hit.onsetSample = p.onset;
            hit.emitOffset = (int) std::clamp<int64_t> (windowEnd - blockStart, 0, n - 1);
            extractor.extract (windowScratch.data(), window, settings, hit.features, hit.peakDb);
            hit.result = canClassify ? model->classify (hit.features) : Classification {};
        }

        pendingCount = kept;
    }

    return numOut;
}

} // namespace bbr
