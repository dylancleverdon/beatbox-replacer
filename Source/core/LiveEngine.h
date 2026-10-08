#pragma once

#include "Classifier.h"
#include "FeatureExtractor.h"
#include "OnsetDetector.h"
#include "Types.h"

#include <vector>

namespace bbr
{

struct LiveHit
{
    int64_t onsetSample = 0;   // engine clock (samples since reset()) at which the hit started
    int emitOffset = 0;        // offset inside the CURRENT block at which the analysis window
                               // completed -- the earliest point a MIDI note can be sent
    Classification result;     // kUnassigned when no model is loaded
    float peakDb = -120.0f;
    Features features {};
};

// Real-time hit detection + classification for the plugin's audio thread.
//
// Keeps a ring buffer of the incoming mono signal, runs OnsetDetector on it, and for every onset
// waits until the analysis window (model's FeatureSettings, or `fallbackFeatures` when no model)
// has fully arrived, then extracts features, classifies, and reports a LiveHit.
// A pending onset remembers the window length it was detected with.
//
// Equivalence guarantee (tested): for the same signal and settings, the onsets, features and
// classifications reported here match analyzeBuffer() exactly, whatever the block size.
class LiveEngine
{
public:
    // Allocates. maxBlockSize is the largest n process() will be called with.
    void prepare (double sampleRate, int maxBlockSize);

    // Clears state; engine clock restarts at 0. Real-time safe.
    void reset() noexcept;

    // Real-time safe.
    void setDetectorSettings (const DetectorSettings& s) noexcept { detector.setSettings (s); }
    void setFallbackFeatureSettings (const FeatureSettings& s) noexcept { fallbackFeatures = s; }

    // Processes one block of mono samples. Writes completed hits (in order) into `out`, up to
    // maxOut, and returns the count. `model` may be null or empty. Real-time safe.
    // If n > maxBlockSize the block is processed in chunks internally.
    int process (const float* mono, int n, const ClassifierModel* model, LiveHit* out, int maxOut) noexcept;

    int64_t getSamplesProcessed() const noexcept { return clock; }
    double getSampleRate() const noexcept { return sampleRate; }

private:
    struct Pending
    {
        int64_t onset;
        int windowSamples;
    };

    double sampleRate = 48000.0;
    int maxBlock = 512;
    OnsetDetector detector;
    FeatureExtractor extractor;
    FeatureSettings fallbackFeatures;

    std::vector<float> ring;   // power-of-two length
    int ringMask = 0;
    int64_t clock = 0;         // samples written to ring

    std::vector<Pending> pending; // fixed capacity, used as a FIFO
    int pendingCount = 0;
    std::vector<int64_t> onsetScratch;
    std::vector<float> windowScratch; // contiguous copy of a window from the ring
};

} // namespace bbr
