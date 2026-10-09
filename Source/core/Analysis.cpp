#include "Analysis.h"

#include "FeatureExtractor.h"
#include "OnsetDetector.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace bbr
{

namespace
{
    constexpr int kAnalysisBlockSize = 512;
    constexpr int kMaxOnsetsPerBlock = kAnalysisBlockSize / 16 + 2; // hop is at least 16 samples

    // Extracts features from `available` samples at x, zero-padded to the full window.
    void extractPadded (FeatureExtractor& extractor, const float* x, int64_t available,
                        const FeatureSettings& settings, std::vector<float>& padded,
                        Features& featuresOut, float& peakDbOut)
    {
        const int window = extractor.windowSamples (settings);
        const int64_t count = std::clamp<int64_t> (available, 0, window);

        padded.assign ((size_t) window, 0.0f);

        if (x != nullptr)
            std::copy (x, x + count, padded.begin());

        extractor.extract (padded.data(), window, settings, featuresOut, peakDbOut);
    }
}

std::vector<DetectedHit> analyzeBuffer (const float* mono, int64_t numSamples, double sampleRate,
                                        const DetectorSettings& detectorSettings,
                                        const FeatureSettings& featureSettings)
{
    std::vector<DetectedHit> hits;

    if (mono == nullptr || numSamples <= 0 || ! (sampleRate > 0.0))
        return hits;

    OnsetDetector detector;
    detector.prepare (sampleRate);
    detector.setSettings (detectorSettings);

    std::vector<int64_t> onsets;
    int64_t found[kMaxOnsetsPerBlock];

    for (int64_t pos = 0; pos < numSamples; pos += kAnalysisBlockSize)
    {
        const int n = (int) std::min<int64_t> (kAnalysisBlockSize, numSamples - pos);
        const int count = detector.process (mono + pos, n, found, kMaxOnsetsPerBlock);
        onsets.insert (onsets.end(), found, found + count);
    }

    FeatureExtractor extractor;
    extractor.prepare (sampleRate);
    std::vector<float> padded;
    hits.reserve (onsets.size());

    for (const int64_t onset : onsets)
    {
        DetectedHit hit;
        hit.onsetSample = onset;
        extractPadded (extractor, mono + onset, numSamples - onset, featureSettings, padded,
                       hit.features, hit.peakDb);
        hits.push_back (hit);
    }

    return hits;
}

std::vector<float> makeSnippet (const float* mono, int64_t numSamples, int64_t onsetSample,
                                double sampleRate, int& onsetOffsetOut)
{
    onsetOffsetOut = 0;

    if (mono == nullptr || numSamples <= 0 || ! (sampleRate > 0.0))
        return {};

    const int64_t pre = (int64_t) std::llround ((double) kSnippetPreMs * 0.001 * sampleRate);
    const int64_t post = (int64_t) std::llround ((double) kSnippetPostMs * 0.001 * sampleRate);
    const int64_t start = std::clamp<int64_t> (onsetSample - pre, 0, numSamples);
    const int64_t end = std::clamp<int64_t> (onsetSample + post, start, numSamples);

    onsetOffsetOut = (int) std::clamp<int64_t> (onsetSample - start, 0, end - start);
    return std::vector<float> (mono + start, mono + end);
}

void recomputeFeatures (std::vector<TrainingHit>& hits, const FeatureSettings& settings)
{
    std::map<double, FeatureExtractor> extractors;
    std::vector<float> padded;

    for (TrainingHit& hit : hits)
    {
        if (hit.snippet.empty() || ! (hit.sampleRate > 0.0))
            continue;

        auto [it, inserted] = extractors.try_emplace (hit.sampleRate);

        if (inserted)
            it->second.prepare (hit.sampleRate);

        const int64_t size = (int64_t) hit.snippet.size();
        const int64_t offset = std::clamp<int64_t> (hit.onsetOffset, 0, size);

        extractPadded (it->second, hit.snippet.data() + offset, size - offset, settings, padded,
                       hit.features, hit.peakDb);
    }
}

int velocityFor (float peakDb, float referenceDb, bool dynamic, int fixedVelocity) noexcept
{
    if (! dynamic)
        return std::clamp (fixedVelocity, 1, 127);

    const float reference = std::isnan (referenceDb) ? -18.0f : referenceDb;
    const float velocity = 100.0f + 4.0f * (peakDb - reference);

    if (std::isnan (velocity))
        return 100;

    return (int) std::lround (std::clamp (velocity, 1.0f, 127.0f));
}

} // namespace bbr
