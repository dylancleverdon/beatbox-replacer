// Unit tests for the JUCE-free analysis core. Run: bbr_core_tests [name-filter]

#include "TestSignals.h"

#include "core/Analysis.h"
#include "core/Capture.h"
#include "core/Classifier.h"
#include "core/Clustering.h"
#include "core/FeatureExtractor.h"
#include "core/Fft.h"
#include "core/LearnSession.h"
#include "core/LiveEngine.h"
#include "core/MidiFile.h"
#include "core/OnsetDetector.h"
#include "core/Types.h"
#include "core/Version.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace bbrtest;

// ---------------------------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------------------------
namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

int failures = 0;
int checks = 0;
const char* currentTest = "";

struct Registrar
{
    Registrar (const char* name, std::function<void()> fn) { registry().push_back ({ name, std::move (fn) }); }
};

void fail (const char* file, int line, const std::string& what)
{
    ++failures;
    std::printf ("  FAILED [%s] %s:%d: %s\n", currentTest, file, line, what.c_str());
}

struct RequireFailed {};
} // namespace

#define TEST(name)                                         \
    static void test_##name();                             \
    static Registrar registrar_##name (#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                    \
    do { ++checks; if (! (cond)) fail (__FILE__, __LINE__, #cond); } while (false)

#define CHECK_MSG(cond, msg)                                                          \
    do { ++checks; if (! (cond)) fail (__FILE__, __LINE__, std::string (#cond) + " -- " + (msg)); } while (false)

#define CHECK_NEAR(a, b, tol)                                                                    \
    do {                                                                                         \
        ++checks;                                                                                \
        const double va_ = (double) (a), vb_ = (double) (b);                                     \
        if (! (std::abs (va_ - vb_) <= (double) (tol)))                                          \
            fail (__FILE__, __LINE__, std::string (#a " ~= " #b ": ") + std::to_string (va_)     \
                                          + " vs " + std::to_string (vb_));                      \
    } while (false)

#define REQUIRE(cond)                                                         \
    do { ++checks; if (! (cond)) { fail (__FILE__, __LINE__, #cond); throw RequireFailed {}; } } while (false)

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
namespace
{
int classOf (Sound s)
{
    switch (s)
    {
        case Sound::kick:  return 0;
        case Sound::snare: return 1;
        case Sound::hat:   return 2;
        default:           return bbr::kIgnoreSlotId;
    }
}

std::vector<bbr::SlotInfo> defaultSlots()
{
    return { { 0, "Kick", 36 }, { 1, "Snare", 38 }, { 2, "Hi-hat", 42 } };
}

// For each truth hit, the index of the detected hit within tol samples (or -1).
std::vector<int> matchHits (const std::vector<TruthHit>& truth, const std::vector<bbr::DetectedHit>& hits,
                            int64_t tolSamples)
{
    std::vector<int> match (truth.size(), -1);
    std::vector<bool> used (hits.size(), false);

    for (size_t t = 0; t < truth.size(); ++t)
    {
        int64_t bestDist = tolSamples + 1;
        for (size_t h = 0; h < hits.size(); ++h)
        {
            const auto d = std::abs (hits[h].onsetSample - truth[t].sample);
            if (! used[h] && d < bestDist)
            {
                bestDist = d;
                match[t] = (int) h;
            }
        }
        if (match[t] >= 0)
            used[(size_t) match[t]] = true;
    }
    return match;
}

// Labels detected hits from the ground truth and turns them into training hits.
std::vector<bbr::TrainingHit> trainingFromPattern (const Pattern& p, const bbr::FeatureSettings& fs,
                                                   const bbr::DetectorSettings& ds = {})
{
    const auto hits = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), p.sampleRate, ds, fs);
    const auto match = matchHits (p.truth, hits, (int64_t) (0.005 * p.sampleRate));

    std::vector<bbr::TrainingHit> out;
    for (size_t t = 0; t < p.truth.size(); ++t)
    {
        if (match[t] < 0)
            continue;

        const auto& h = hits[(size_t) match[t]];
        bbr::TrainingHit th;
        th.slotId = classOf (p.truth[t].sound);
        th.sampleRate = p.sampleRate;
        th.snippet = bbr::makeSnippet (p.audio.data(), (int64_t) p.audio.size(), h.onsetSample, p.sampleRate, th.onsetOffset);
        th.peakDb = h.peakDb;
        th.features = h.features;
        out.push_back (std::move (th));
    }
    return out;
}

bool featuresFinite (const bbr::Features& f)
{
    return std::all_of (f.begin(), f.end(), [] (float v) { return std::isfinite (v); });
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------------------------
TEST (note_names)
{
    CHECK (bbr::noteName (36) == "C1");
    CHECK (bbr::noteName (60) == "C3");
    CHECK (bbr::noteName (42) == "F#1");
    CHECK (bbr::noteName (0) == "C-2");
    CHECK (bbr::noteName (127) == "G8");

    for (int n = 0; n < 128; ++n)
        CHECK_MSG (bbr::parseNoteName (bbr::noteName (n)) == n, bbr::noteName (n));

    CHECK (bbr::parseNoteName ("Db1") == 37);
    CHECK (bbr::parseNoteName ("c1") == 36);
    CHECK (bbr::parseNoteName ("hello") == -1);
    CHECK (bbr::parseNoteName ("") == -1);
    CHECK (bbr::parseNoteName ("C") == -1);
    CHECK (bbr::parseNoteName ("G#8") == -1);  // 128
    CHECK (bbr::parseNoteName ("C-3") == -1);  // -12
}

// ---------------------------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------------------------
TEST (fft_matches_naive_dft)
{
    const int n = 256;
    bbr::Fft fft;
    fft.prepare (n);
    REQUIRE (fft.getSize() == n);

    Rng rng (3);
    std::vector<std::complex<float>> data ((size_t) n), ref ((size_t) n);
    for (auto& c : data)
        c = { (float) rng.uniform (-1, 1), (float) rng.uniform (-1, 1) };

    for (int k = 0; k < n; ++k)
    {
        std::complex<double> acc;
        for (int t = 0; t < n; ++t)
            acc += std::complex<double> (data[(size_t) t]) * std::polar (1.0, -2.0 * kPi * k * t / n);
        ref[(size_t) k] = std::complex<float> (acc);
    }

    fft.perform (data.data());

    double maxErr = 0, maxMag = 0;
    for (int k = 0; k < n; ++k)
    {
        maxErr = std::max (maxErr, (double) std::abs (data[(size_t) k] - ref[(size_t) k]));
        maxMag = std::max (maxMag, (double) std::abs (ref[(size_t) k]));
    }
    CHECK (maxErr <= 1.0e-3 * maxMag);
}

TEST (fft_impulse_and_sine)
{
    const int n = 512;
    bbr::Fft fft;
    fft.prepare (n);
    std::vector<float> in ((size_t) n, 0.0f), mags ((size_t) n / 2 + 1);
    std::vector<std::complex<float>> scratch ((size_t) n);

    in[0] = 1.0f;
    fft.performRealMagnitudes (in.data(), mags.data(), scratch.data());
    for (auto m : mags)
        CHECK_NEAR (m, 1.0, 1.0e-4);

    const int bin = 37;
    for (int i = 0; i < n; ++i)
        in[(size_t) i] = (float) std::sin (2.0 * kPi * bin * i / n);
    fft.performRealMagnitudes (in.data(), mags.data(), scratch.data());
    CHECK ((int) (std::max_element (mags.begin(), mags.end()) - mags.begin()) == bin);
    CHECK_NEAR (mags[(size_t) bin], n / 2.0, 0.5);

    CHECK (bbr::Fft::nextPowerOfTwo (300) == 512);
    CHECK (bbr::Fft::nextPowerOfTwo (256) == 256);
}

// ---------------------------------------------------------------------------------------------
// Onset detection
// ---------------------------------------------------------------------------------------------
static void checkDetection (double sampleRate, uint64_t seed)
{
    const auto p = makeMixedPattern (sampleRate, seed, 14, 13, 13, 0, 0.12, 0.3);
    const auto hits = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sampleRate, {}, {});
    const auto match = matchHits (p.truth, hits, (int64_t) (0.005 * sampleRate));

    const auto found = (int) std::count_if (match.begin(), match.end(), [] (int m) { return m >= 0; });
    CHECK_MSG (found == (int) p.truth.size(),
               std::to_string (found) + "/" + std::to_string (p.truth.size()) + " at " + std::to_string (sampleRate));
    CHECK_MSG (hits.size() == p.truth.size(), "detected " + std::to_string (hits.size()));
}

TEST (onsets_found_44k) { checkDetection (44100.0, 11); }
TEST (onsets_found_48k) { checkDetection (48000.0, 12); }
TEST (onsets_found_96k) { checkDetection (96000.0, 13); }

TEST (onsets_block_size_invariant)
{
    const double sr = 48000.0;
    const auto p = makeMixedPattern (sr, 21, 8, 8, 8);
    std::vector<std::vector<int64_t>> results;

    for (int block : { 1, 37, 64, 512, (int) p.audio.size() })
    {
        bbr::OnsetDetector d;
        d.prepare (sr);
        std::vector<int64_t> onsets, scratch (64);
        for (size_t pos = 0; pos < p.audio.size(); pos += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, p.audio.size() - pos);
            const int got = d.process (p.audio.data() + pos, n, scratch.data(), (int) scratch.size());
            onsets.insert (onsets.end(), scratch.begin(), scratch.begin() + got);
        }
        CHECK (d.getSamplesProcessed() == (int64_t) p.audio.size());
        results.push_back (onsets);
    }

    for (size_t i = 1; i < results.size(); ++i)
        CHECK (results[i] == results[0]);
    CHECK (results[0].size() == p.truth.size());
}

TEST (onsets_silence_and_noise)
{
    const double sr = 48000.0;
    std::vector<float> silence ((size_t) sr * 2, 0.0f);
    CHECK (bbr::analyzeBuffer (silence.data(), (int64_t) silence.size(), sr, {}, {}).empty());

    const auto noise = whiteNoise ((int64_t) sr * 2, -70.0f, 5);
    CHECK (bbr::analyzeBuffer (noise.data(), (int64_t) noise.size(), sr, {}, {}).empty());
}

TEST (onsets_min_gap)
{
    const double sr = 48000.0;
    const auto p = buildPattern ({ { 0.2, Sound::snare }, { 0.23, Sound::hat } }, sr, 7);
    bbr::DetectorSettings ds;
    ds.minGapMs = 60.0f;
    CHECK (bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sr, ds, {}).size() == 1);
}

TEST (onsets_quiet_hat)
{
    const double sr = 48000.0;
    // peak -40 dBFS
    const auto p = buildPattern ({ { 0.3, Sound::hat, 0.01f } }, sr, 8, { -90.0f, 0.4, false, 0.0f });
    const auto hits = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sr, {}, {});
    REQUIRE (hits.size() == 1);
    CHECK (std::abs (hits[0].onsetSample - p.truth[0].sample) < (int64_t) (0.005 * sr));
}

TEST (onsets_sustained_tone_single)
{
    const double sr = 48000.0;
    std::vector<float> x ((size_t) sr * 2, 0.0f);
    for (size_t i = (size_t) (0.5 * sr); i < x.size(); ++i)
        x[i] = 0.3f * (float) std::sin (2.0 * kPi * 60.0 * (double) i / sr);
    CHECK (bbr::analyzeBuffer (x.data(), (int64_t) x.size(), sr, {}, {}).size() == 1);
}

// ---------------------------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------------------------
static bbr::Features featuresOf (Sound s, double sr, float gain = 1.0f, uint64_t seed = 1)
{
    const auto p = buildPattern ({ { 0.1, s } }, sr, seed, { -200.0f, 0.3, false, 20.0f * std::log10 (gain) });
    bbr::FeatureExtractor fx;
    fx.prepare (sr);
    bbr::Features f {};
    float peak = 0;
    const auto start = (size_t) p.truth[0].sample;
    fx.extract (p.audio.data() + start, (int) (p.audio.size() - start), {}, f, peak);
    return f;
}

TEST (features_ordering)
{
    const double sr = 48000.0;
    const auto k = featuresOf (Sound::kick, sr), s = featuresOf (Sound::snare, sr), h = featuresOf (Sound::hat, sr);
    CHECK (featuresFinite (k) && featuresFinite (s) && featuresFinite (h));
    CHECK (k[bbr::kFeatCentroid] < s[bbr::kFeatCentroid]);
    CHECK (s[bbr::kFeatCentroid] < h[bbr::kFeatCentroid]);
    CHECK (k[bbr::kFeatLowRatio] > h[bbr::kFeatLowRatio]);
    CHECK (h[bbr::kFeatHighRatio] > k[bbr::kFeatHighRatio]);
}

TEST (features_gain_invariant)
{
    const double sr = 48000.0;
    for (auto s : { Sound::kick, Sound::snare, Sound::hat })
    {
        const auto a = featuresOf (s, sr, 1.0f), b = featuresOf (s, sr, 0.25f);
        for (int i = 0; i < bbr::kNumFeatures; ++i)
            if (i != bbr::kFeatZcr) // zero crossings of the -200 dB-free signal are gain-independent too
                CHECK_NEAR (a[(size_t) i], b[(size_t) i], 1.0e-3);
        CHECK_NEAR (a[bbr::kFeatZcr], b[bbr::kFeatZcr], 1.0e-6);
    }
}

TEST (features_sample_rate_independent)
{
    for (auto s : { Sound::kick, Sound::snare, Sound::hat })
    {
        // Same continuous-time waveform at both rates.
        const auto a44 = synthesizeRateIndependent (44100.0, 0.1, 200.0, 12000.0, 60, 30.0, 0.5f, 99 + (uint64_t) s);
        const auto a48 = synthesizeRateIndependent (48000.0, 0.1, 200.0, 12000.0, 60, 30.0, 0.5f, 99 + (uint64_t) s);
        bbr::FeatureExtractor f44, f48;
        f44.prepare (44100.0);
        f48.prepare (48000.0);
        bbr::Features x {}, y {};
        float px = 0, py = 0;
        f44.extract (a44.data(), (int) a44.size(), {}, x, px);
        f48.extract (a48.data(), (int) a48.size(), {}, y, py);

        double meanAbs = 0;
        for (int i = 0; i < bbr::kNumBands; ++i)
            meanAbs += std::abs (x[(size_t) i] - y[(size_t) i]);
        meanAbs /= bbr::kNumBands;
        CHECK_MSG (meanAbs < 1.5, std::to_string (meanAbs));
        CHECK_NEAR (x[bbr::kFeatCentroid], y[bbr::kFeatCentroid], 0.15);
        CHECK_NEAR (px, py, 0.5);
    }
}

// ---------------------------------------------------------------------------------------------
// Classifier
// ---------------------------------------------------------------------------------------------
TEST (classifier_accuracy)
{
    const double sr = 48000.0;
    const bbr::FeatureSettings fs;
    const auto train = trainingFromPattern (makeMixedPattern (sr, 101, 15, 15, 15), fs);
    REQUIRE (train.size() >= 40);

    const auto model = bbr::ClassifierModel::build (train, fs);
    REQUIRE (model && ! model->empty());
    CHECK (model->countFor (0) >= 13 && model->countFor (1) >= 13 && model->countFor (2) >= 13);
    CHECK (std::isfinite (model->getMedianPeakDb (0)));
    CHECK (std::isnan (model->getMedianPeakDb (5)));

    for (double testRate : { 48000.0, 44100.0 })
    {
        const auto test = makeMixedPattern (testRate, 202, 20, 20, 20, 0, 0.15, 0.3, { -70.0f, 0.4, true, -4.0f });
        const auto hits = bbr::analyzeBuffer (test.audio.data(), (int64_t) test.audio.size(), testRate, {}, fs);
        const auto match = matchHits (test.truth, hits, (int64_t) (0.005 * testRate));

        int correct = 0;
        for (size_t t = 0; t < test.truth.size(); ++t)
            if (match[t] >= 0 && model->classify (hits[(size_t) match[t]].features).slotId == classOf (test.truth[t].sound))
                ++correct;

        CHECK_MSG (correct >= (int) (0.95 * (double) test.truth.size()),
                   std::to_string (correct) + "/" + std::to_string (test.truth.size()) + " at " + std::to_string (testRate));
    }
}

TEST (classifier_empty_and_ignore)
{
    const bbr::FeatureSettings fs;
    const auto empty = bbr::ClassifierModel::build ({}, fs);
    REQUIRE (empty != nullptr);
    CHECK (empty->empty());
    CHECK (empty->classify (bbr::Features {}).slotId == bbr::kUnassigned);

    const double sr = 48000.0;
    const auto train = trainingFromPattern (makeMixedPattern (sr, 303, 10, 10, 10, 10), fs);
    const auto model = bbr::ClassifierModel::build (train, fs);
    CHECK (model->countFor (bbr::kIgnoreSlotId) >= 8);

    const auto test = makeMixedPattern (sr, 404, 0, 0, 0, 12);
    const auto hits = bbr::analyzeBuffer (test.audio.data(), (int64_t) test.audio.size(), sr, {}, fs);
    int ignored = 0;
    for (const auto& h : hits)
        if (model->classify (h.features).slotId == bbr::kIgnoreSlotId)
            ++ignored;
    CHECK_MSG (hits.size() >= 10 && ignored >= (int) hits.size() - 1,
               std::to_string (ignored) + "/" + std::to_string (hits.size()));
}

// ---------------------------------------------------------------------------------------------
// Clustering
// ---------------------------------------------------------------------------------------------
static std::pair<std::vector<bbr::Features>, std::vector<int>> clusterData (uint64_t seed)
{
    const double sr = 48000.0;
    const auto p = makeMixedPattern (sr, seed, 12, 12, 12);
    const auto hits = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sr, {}, {});
    const auto match = matchHits (p.truth, hits, (int64_t) (0.005 * sr));
    std::vector<bbr::Features> data;
    std::vector<int> truth;
    for (size_t t = 0; t < p.truth.size(); ++t)
    {
        if (match[t] < 0)
            continue;
        data.push_back (hits[(size_t) match[t]].features);
        truth.push_back (classOf (p.truth[t].sound));
    }
    return { data, truth };
}

TEST (kmeans_purity_and_order)
{
    const auto [data, truth] = clusterData (505);
    REQUIRE (data.size() >= 30);
    const auto r = bbr::kmeans (data, 3);
    REQUIRE (r.k == 3 && r.labels.size() == data.size());

    // Groups are ordered by centroid: 0 kick-like, 1 snare-like, 2 hat-like.
    int agree = 0;
    for (size_t i = 0; i < data.size(); ++i)
        if (r.labels[i] == truth[i])
            ++agree;
    CHECK_MSG (agree >= (int) (0.95 * (double) data.size()), std::to_string (agree) + "/" + std::to_string (data.size()));

    const auto again = bbr::kmeans (data, 3);
    CHECK (again.labels == r.labels);
    CHECK (r.silhouette > 0.2f);
}

TEST (auto_cluster)
{
    const auto [data, truth] = clusterData (606);
    CHECK (bbr::autoCluster (data, 2, bbr::kMaxSlots, 3).k == 3);

    CHECK (bbr::autoCluster ({}).k == 0);
    CHECK (bbr::autoCluster ({ data[0] }).k == 1);
    const auto two = bbr::autoCluster ({ data[0], data[1] });
    CHECK (two.k == 1 && two.labels.size() == 2);
    CHECK (bbr::kmeans ({}, 3).k == 0);
    CHECK (bbr::kmeans ({ data[0], data[1] }, 5).k == 2);
}

TEST (suggest_group_slots)
{
    const auto [data, truth] = clusterData (707);
    const auto r = bbr::kmeans (data, 3);
    std::vector<std::vector<bbr::Features>> groups (3);
    for (size_t i = 0; i < data.size(); ++i)
        groups[(size_t) r.labels[i]].push_back (data[i]);

    // Name heuristic (slots listed in a scrambled order on purpose).
    std::vector<bbr::SlotInfo> slots { { 4, "Snare", 38 }, { 1, "Hi-Hat", 42 }, { 6, "KICK", 36 } };
    const auto heuristic = bbr::suggestGroupSlots (groups, nullptr, slots);
    REQUIRE (heuristic.size() == 3);
    CHECK (heuristic[0] == 6);
    CHECK (heuristic[1] == 4);
    CHECK (heuristic[2] == 1);

    // Model mode: majority vote.
    const bbr::FeatureSettings fs;
    const auto model = bbr::ClassifierModel::build (trainingFromPattern (makeMixedPattern (48000.0, 808, 12, 12, 12), fs), fs);
    const auto voted = bbr::suggestGroupSlots (groups, model.get(), defaultSlots());
    CHECK (voted == (std::vector<int> { 0, 1, 2 }));
}

// ---------------------------------------------------------------------------------------------
// Learn session
// ---------------------------------------------------------------------------------------------
TEST (learn_session)
{
    const double sr = 48000.0;
    auto p = makeMixedPattern (sr, 909, 10, 10, 10);
    const auto truthCount = p.truth.size();

    bbr::LearnSession s;
    s.setAudio (p.audio, sr);
    s.analyze ({}, {}, defaultSlots(), nullptr);
    REQUIRE (s.numHits() == (int) truthCount);
    REQUIRE (s.numGroups() == 3);
    CHECK (s.getGroupSlot (0) == 0);
    CHECK (s.getGroupSlot (1) == 1);
    CHECK (s.getGroupSlot (2) == 2);

    int total = 0;
    for (int g = 0; g < s.numGroups(); ++g)
    {
        total += s.groupSize (g);
        const int rep = s.representativeHit (g);
        CHECK (rep >= 0 && rep < s.numHits() && s.groupOfHit (rep) == g);
    }
    CHECK (total == s.numHits());

    // Overrides and group changes
    CHECK (! s.hasHitOverride (0));
    s.setHitOverride (0, bbr::kIgnoreSlotId);
    CHECK (s.hasHitOverride (0) && s.getHitSlot (0) == bbr::kIgnoreSlotId);
    s.setHitOverride (0, bbr::kUnassigned);
    CHECK (! s.hasHitOverride (0) && s.getHitSlot (0) == s.getGroupSlot (s.groupOfHit (0)));

    s.setHitOverride (1, 2);
    const int g1 = s.groupOfHit (1);
    s.setGroupSlot (g1, bbr::kUnassigned);
    CHECK (s.getHitSlot (1) == 2);

    auto counts = s.slotCounts();
    REQUIRE (counts.size() == (size_t) bbr::kMaxSlots + 2);
    CHECK (counts[(size_t) bbr::kMaxSlots + 1] == s.groupSize (g1) - 1);

    const auto training = s.makeTrainingHits();
    CHECK ((int) training.size() == s.numHits() - (s.groupSize (g1) - 1));
    const int expectedLen = (int) std::lround ((bbr::kSnippetPreMs + bbr::kSnippetPostMs) * sr / 1000.0);
    for (const auto& t : training)
    {
        CHECK (t.slotId != bbr::kUnassigned);
        CHECK (std::abs ((int) t.snippet.size() - expectedLen) <= 2);
        CHECK (std::abs (t.onsetOffset - (int) std::lround (bbr::kSnippetPreMs * sr / 1000.0)) <= 1);
    }

    // forgetSlot clears assignments pointing at the slot
    s.forgetSlot (2);
    CHECK (s.getHitSlot (1) == bbr::kUnassigned);
    for (int g = 0; g < s.numGroups(); ++g)
        CHECK (s.getGroupSlot (g) != 2);

    // recluster keeps overrides
    s.setHitOverride (3, 0);
    s.recluster (2, defaultSlots(), nullptr);
    CHECK (s.numGroups() == 2);
    CHECK (s.getHitSlot (3) == 0);

    s.clear();
    CHECK (! s.hasAudio() && s.numHits() == 0);
}

TEST (recompute_features)
{
    const double sr = 44100.0;
    const auto p = makeMixedPattern (sr, 1001, 4, 4, 4);
    bbr::LearnSession s;
    s.setAudio (p.audio, sr);
    s.analyze ({}, {}, defaultSlots(), nullptr);
    auto hits = s.makeTrainingHits();
    REQUIRE (! hits.empty());
    const auto original = hits;

    bbr::recomputeFeatures (hits, {});
    for (size_t i = 0; i < hits.size(); ++i)
        for (int f = 0; f < bbr::kNumFeatures; ++f)
            CHECK_NEAR (hits[i].features[(size_t) f], original[i].features[(size_t) f], 1.0e-4);

    bbr::FeatureSettings longer;
    longer.windowMs = 45.0f;
    bbr::recomputeFeatures (hits, longer);
    double diff = 0;
    for (size_t i = 0; i < hits.size(); ++i)
        diff += std::abs (hits[i].features[bbr::kFeatEnvelope] - original[i].features[bbr::kFeatEnvelope]);
    CHECK (diff > 1.0e-3);
}

// ---------------------------------------------------------------------------------------------
// Live engine == offline analysis
// ---------------------------------------------------------------------------------------------
TEST (live_engine_matches_offline)
{
    const double sr = 48000.0;
    const auto p = makeMixedPattern (sr, 1111, 8, 8, 8);
    const bbr::FeatureSettings fs;
    const auto offline = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sr, {}, fs);

    bbr::FeatureExtractor fx;
    fx.prepare (sr);
    const int window = fx.windowSamples (fs);

    for (int block : { 1, 64, 480, 4096 })
    {
        bbr::LiveEngine engine;
        engine.prepare (sr, block == 4096 ? 512 : block);
        engine.setFallbackFeatureSettings (fs);

        std::vector<bbr::LiveHit> live, out (64);
        for (size_t pos = 0; pos < p.audio.size(); pos += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, p.audio.size() - pos);
            const int got = engine.process (p.audio.data() + pos, n, nullptr, out.data(), (int) out.size());
            for (int i = 0; i < got; ++i)
            {
                // The window is complete once sample onset + window - 1 has arrived; the note goes at
                // onset + window, or on the block's last sample if that's where the window ended.
                const auto due = out[(size_t) i].onsetSample + window - (int64_t) pos;
                CHECK (out[(size_t) i].emitOffset == (int) std::min (due, (int64_t) n - 1));
                CHECK (out[(size_t) i].result.slotId == bbr::kUnassigned);
            }
            live.insert (live.end(), out.begin(), out.begin() + got);
        }

        std::vector<bbr::DetectedHit> complete;
        for (const auto& h : offline)
            if (h.onsetSample + window <= (int64_t) p.audio.size())
                complete.push_back (h);

        REQUIRE (live.size() == complete.size());
        for (size_t i = 0; i < live.size(); ++i)
        {
            CHECK (live[i].onsetSample == complete[i].onsetSample);
            CHECK_NEAR (live[i].peakDb, complete[i].peakDb, 1.0e-4);
            for (int f = 0; f < bbr::kNumFeatures; ++f)
                CHECK_NEAR (live[i].features[(size_t) f], complete[i].features[(size_t) f], 1.0e-4);
        }
    }
}

TEST (live_engine_with_model)
{
    const double sr = 44100.0;
    bbr::FeatureSettings fs;
    fs.windowMs = 25.0f;
    const auto model = bbr::ClassifierModel::build (trainingFromPattern (makeMixedPattern (sr, 1212, 12, 12, 12), fs), fs);
    const auto p = makeMixedPattern (sr, 1313, 6, 6, 6);
    const auto offline = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sr, {}, fs);

    bbr::LiveEngine engine;
    engine.prepare (sr, 256);
    std::vector<bbr::LiveHit> out (64);
    size_t idx = 0;
    for (size_t pos = 0; pos < p.audio.size(); pos += 256)
    {
        const int n = (int) std::min ((size_t) 256, p.audio.size() - pos);
        const int got = engine.process (p.audio.data() + pos, n, model.get(), out.data(), (int) out.size());
        for (int i = 0; i < got; ++i, ++idx)
        {
            REQUIRE (idx < offline.size());
            CHECK (out[(size_t) i].onsetSample == offline[idx].onsetSample);
            CHECK (out[(size_t) i].result.slotId == model->classify (offline[idx].features).slotId);
        }
    }
    CHECK (idx == offline.size());
}

// ---------------------------------------------------------------------------------------------
// Velocity, capture, MIDI
// ---------------------------------------------------------------------------------------------
TEST (velocity)
{
    CHECK (bbr::velocityFor (-10.0f, -20.0f, false, 90) == 90);
    CHECK (bbr::velocityFor (-10.0f, -20.0f, false, 0) == 1);
    CHECK (bbr::velocityFor (-10.0f, -20.0f, false, 300) == 127);
    CHECK (bbr::velocityFor (-20.0f, -20.0f, true, 64) == 100);
    CHECK (bbr::velocityFor (-14.0f, -20.0f, true, 64) == 124);
    CHECK (bbr::velocityFor (0.0f, -40.0f, true, 64) == 127);
    CHECK (bbr::velocityFor (-100.0f, -20.0f, true, 64) == 1);
    CHECK (bbr::velocityFor (-18.0f, std::numeric_limits<float>::quiet_NaN(), true, 64) == 100);
}

TEST (capture_helpers)
{
    CHECK_NEAR (bbr::quarterNotesPerBar (4, 4), 4.0, 1e-12);
    CHECK_NEAR (bbr::quarterNotesPerBar (6, 8), 3.0, 1e-12);
    CHECK_NEAR (bbr::quarterNotesPerBar (3, 4), 3.0, 1e-12);
    CHECK (bbr::barNumberAt (0.0, 4, 4) == 1);
    CHECK (bbr::barNumberAt (3.99, 4, 4) == 1);
    CHECK (bbr::barNumberAt (16.0, 4, 4) == 5);
    CHECK (bbr::barNumberAt (-2.0, 4, 4) == 1);

    bbr::CaptureTake take;
    take.startPpq = 17.0;
    take.barStartPpq = 16.0;
    take.bpm = 120.0;
    take.hits = { { 15.5, 0, -20.0f, {} },   // before the clip start: dropped in bar mode
                  { 17.0, 0, -20.0f, {} },
                  { 17.5, 1, -14.0f, {} },
                  { 18.0, bbr::kIgnoreSlotId, -20.0f, {} },
                  { 18.5, bbr::kUnassigned, -20.0f, {} },
                  { 19.0, 3, -20.0f, {} },   // slot without a note
                  { 19.5, 2, -20.0f, {} } };

    const int notes[bbr::kMaxSlots] = { 36, 38, 42, -1, -1, -1, -1, -1 };
    bbr::TakeToMidiOptions opt;
    opt.dynamicVelocity = false;
    opt.fixedVelocity = 99;
    auto midi = bbr::takeToNotes (take, notes, nullptr, opt);
    REQUIRE (midi.size() == 3);
    CHECK_NEAR (midi[0].startBeats, 1.0, 1e-9);
    CHECK (midi[0].note == 36 && midi[1].note == 38 && midi[2].note == 42);
    CHECK (midi[0].velocity == 99);
    CHECK_NEAR (midi[0].lengthBeats, 0.25, 1e-9);

    opt.fromSongStart = true;
    opt.dynamicVelocity = true;
    midi = bbr::takeToNotes (take, notes, nullptr, opt);
    REQUIRE (midi.size() == 4);
    CHECK_NEAR (midi[0].startBeats, 15.5, 1e-9);
    CHECK (midi[2].velocity == 116); // -14 dB vs the default -18 dB reference
}

TEST (reclassify_take)
{
    const double sr = 48000.0;
    const bbr::FeatureSettings fs;
    const auto model = bbr::ClassifierModel::build (trainingFromPattern (makeMixedPattern (sr, 1414, 10, 10, 10), fs), fs);
    const auto p = makeMixedPattern (sr, 1515, 4, 4, 4);
    const auto hits = bbr::analyzeBuffer (p.audio.data(), (int64_t) p.audio.size(), sr, {}, fs);

    bbr::CaptureTake take;
    for (const auto& h : hits)
        take.hits.push_back ({ 0.0, bbr::kUnassigned, h.peakDb, h.features });
    bbr::reclassifyTake (take, *model);
    for (size_t i = 0; i < hits.size(); ++i)
        CHECK (take.hits[i].slotId == model->classify (hits[i].features).slotId);
}

TEST (midi_round_trip)
{
    std::vector<bbr::MidiNote> notes { { 0.0, 0.25, 36, 100, 0 }, { 1.5, 0.25, 38, 80, 0 },
                                       { 1.5, 0.125, 42, 64, 0 }, { 3.75, 0.25, 36, 127, 9 } };
    const auto bytes = bbr::writeMidiFile (notes, 93.0, 4, 4, 960, "Beatbox take");
    const auto parsed = bbr::parseMidiFile (bytes);
    REQUIRE (parsed.ok);
    CHECK (parsed.format == 0 && parsed.numTracks == 1 && parsed.ppq == 960);
    CHECK (parsed.trackName == "Beatbox take");
    CHECK (parsed.bpm == 0.0);             // no tempo event by default
    CHECK (parsed.timeSigNum == 0);
    REQUIRE (parsed.notes.size() == 4);
    CHECK (parsed.notes[0].startTick == 0 && parsed.notes[0].endTick == 240 && parsed.notes[0].note == 36);
    CHECK (parsed.notes[3].startTick == 3600 && parsed.notes[3].channel == 9 && parsed.notes[3].velocity == 127);

    const auto withTempo = bbr::parseMidiFile (bbr::writeMidiFile (notes, 93.0, 6, 8, 480, "x", true));
    REQUIRE (withTempo.ok);
    CHECK_NEAR (withTempo.bpm, 93.0, 0.01);
    CHECK (withTempo.timeSigNum == 6 && withTempo.timeSigDen == 8);
    CHECK (withTempo.notes[1].startTick == 720);
}

TEST (midi_retrigger_order)
{
    // A note ending exactly where the next one starts must not be cut short.
    std::vector<bbr::MidiNote> notes { { 0.0, 0.5, 36, 100, 0 }, { 0.5, 0.5, 36, 90, 0 } };
    const auto parsed = bbr::parseMidiFile (bbr::writeMidiFile (notes, 120.0));
    REQUIRE (parsed.ok && parsed.notes.size() == 2);
    CHECK (parsed.notes[0].endTick == 480 && parsed.notes[1].startTick == 480 && parsed.notes[1].endTick == 960);
}

TEST (midi_parser_robust)
{
    std::vector<bbr::MidiNote> notes;
    for (int i = 0; i < 20; ++i)
        notes.push_back ({ i * 0.5, 0.25, 36 + (i % 3) * 3, 100, 0 });
    const auto bytes = bbr::writeMidiFile (notes, 120.0);

    for (size_t len = 0; len < bytes.size(); len += 3)
        (void) bbr::parseMidiFile (std::vector<uint8_t> (bytes.begin(), bytes.begin() + (long) len));

    CHECK (! bbr::parseMidiFile ({}).ok);
    CHECK (! bbr::parseMidiFile ({ 'M', 'T', 'h', 'd' }).ok);

    Rng rng (77);
    for (int i = 0; i < 2000; ++i)
    {
        auto b = bytes;
        const int edits = rng.uniformInt (1, 8);
        for (int e = 0; e < edits; ++e)
            b[(size_t) rng.uniformInt (0, (int) b.size() - 1)] = (uint8_t) rng.uniformInt (0, 255);
        (void) bbr::parseMidiFile (b);
    }
    CHECK (true);
}

// ---------------------------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------------------------
TEST (versions)
{
    const auto v = bbr::Version::parse ("v1.0.42");
    REQUIRE (v.has_value());
    CHECK (v->major == 1 && v->minor == 0 && v->patch == 42);
    CHECK (v->toString() == "1.0.42");
    CHECK (bbr::Version::parse ("1.2")->patch == 0);
    CHECK (bbr::Version::parse (" 2.0.0-beta ")->major == 2);
    CHECK (! bbr::Version::parse ("abc").has_value());
    CHECK (! bbr::Version::parse ("").has_value());

    CHECK (bbr::compareVersions (*bbr::Version::parse ("1.0.10"), *bbr::Version::parse ("1.0.9")) > 0);
    CHECK (bbr::isNewerVersion ("v1.0.43", "1.0.42"));
    CHECK (! bbr::isNewerVersion ("1.0.42", "1.0.42"));
    CHECK (! bbr::isNewerVersion ("1.0.41", "1.0.42"));
    CHECK (bbr::isNewerVersion ("0.0.1", "garbage"));
    CHECK (! bbr::isNewerVersion ("garbage", "1.0.0"));
}

// ---------------------------------------------------------------------------------------------
int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;

    for (const auto& t : registry())
    {
        if (filter != nullptr && std::strstr (t.name, filter) == nullptr)
            continue;

        currentTest = t.name;
        const int before = failures;
        try
        {
            t.fn();
        }
        catch (const RequireFailed&)
        {
        }
        catch (const std::exception& e)
        {
            fail (__FILE__, __LINE__, std::string ("exception: ") + e.what());
        }
        ++run;
        std::printf ("%s %s\n", failures == before ? "[ ok ]" : "[FAIL]", t.name);
    }

    std::printf ("\n%d tests, %d checks, %d failures\n", run, checks, failures);
    return failures == 0 ? 0 : 1;
}
