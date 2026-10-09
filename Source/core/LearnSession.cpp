#include "LearnSession.h"

#include "Analysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace bbr
{

namespace
{
bool isValidTarget (int slotId) noexcept
{
    return slotId == kUnassigned || slotId == kIgnoreSlotId || (slotId >= 0 && slotId < kMaxSlots);
}

std::vector<Features> featuresOf (const std::vector<DetectedHit>& detected)
{
    std::vector<Features> result;
    result.reserve (detected.size());

    for (const auto& h : detected)
        result.push_back (h.features);

    return result;
}

// z-score per dimension over a set of hits (std floored at 1e-3, non-finite values -> 0),
// matching the space Clustering works in.
class Standardiser
{
public:
    explicit Standardiser (const std::vector<DetectedHit>& detected)
    {
        for (size_t d = 0; d < (size_t) kNumFeatures; ++d)
        {
            double sum = 0.0;
            size_t count = 0;

            for (const auto& h : detected)
            {
                if (std::isfinite (h.features[d]))
                {
                    sum += (double) h.features[d];
                    ++count;
                }
            }

            mean[d] = count > 0 ? sum / (double) count : 0.0;
            double sumSq = 0.0;

            for (const auto& h : detected)
            {
                if (std::isfinite (h.features[d]))
                {
                    const double diff = (double) h.features[d] - mean[d];
                    sumSq += diff * diff;
                }
            }

            const double sd = count > 0 ? std::sqrt (sumSq / (double) count) : 0.0;
            invStd[d] = 1.0 / std::max (sd, 1.0e-3);
        }
    }

    double operator() (const Features& f, size_t d) const noexcept
    {
        return std::isfinite (f[d]) ? ((double) f[d] - mean[d]) * invStd[d] : 0.0;
    }

private:
    std::array<double, kNumFeatures> mean {}, invStd {};
};
} // namespace

void LearnSession::setAudio (std::vector<float> mono, double newSampleRate)
{
    clear();
    audio = std::move (mono);
    sampleRate = (newSampleRate > 0.0 && std::isfinite (newSampleRate)) ? newSampleRate : 48000.0;
}

void LearnSession::analyze (const DetectorSettings& detector, const FeatureSettings& features,
                            const std::vector<SlotInfo>& slots, const ClassifierModel* existingModel)
{
    featureSettings = features;
    hits.clear();

    if (! audio.empty())
        hits = analyzeBuffer (audio.data(), (int64_t) audio.size(), sampleRate, detector, featureSettings);

    clusters = autoCluster (featuresOf (hits), 2, kMaxSlots, (int) slots.size());
    groupSlots.assign ((size_t) clusters.k, kUnassigned);
    hitOverrides.assign (hits.size(), kUnassigned);
    suggest (slots, existingModel);
}

void LearnSession::recluster (int k, const std::vector<SlotInfo>& slots, const ClassifierModel* existingModel)
{
    hitOverrides.resize (hits.size(), kUnassigned);

    if (hits.empty())
    {
        clusters = ClusterResult();
        groupSlots.clear();
        return;
    }

    clusters = kmeans (featuresOf (hits), std::clamp (k, 1, numHits()));
    groupSlots.assign ((size_t) clusters.k, kUnassigned);
    suggest (slots, existingModel);
}

void LearnSession::clear()
{
    audio = std::vector<float>(); // releases the memory
    hits.clear();
    clusters = ClusterResult();
    groupSlots.clear();
    hitOverrides.clear();
}

int LearnSession::groupSize (int g) const
{
    if (g < 0 || g >= clusters.k)
        return 0;

    return (int) std::count (clusters.labels.begin(), clusters.labels.end(), g);
}

int LearnSession::representativeHit (int g) const
{
    if (g < 0 || g >= clusters.k || clusters.labels.size() != hits.size())
        return -1;

    const Standardiser z (hits);
    std::array<double, kNumFeatures> centre {};
    size_t count = 0;

    for (size_t i = 0; i < hits.size(); ++i)
    {
        if (clusters.labels[i] != g)
            continue;

        for (size_t d = 0; d < centre.size(); ++d)
            centre[d] += z (hits[i].features, d);

        ++count;
    }

    if (count == 0)
        return -1;

    for (auto& c : centre)
        c /= (double) count;

    int best = -1;
    double bestDist = std::numeric_limits<double>::infinity();

    for (size_t i = 0; i < hits.size(); ++i)
    {
        if (clusters.labels[i] != g)
            continue;

        double d2 = 0.0;

        for (size_t d = 0; d < centre.size(); ++d)
        {
            const double diff = z (hits[i].features, d) - centre[d];
            d2 += diff * diff;
        }

        if (best < 0 || d2 < bestDist)
        {
            best = (int) i;
            bestDist = d2;
        }
    }

    return best;
}

void LearnSession::setGroupSlot (int g, int slotId)
{
    if (g < 0 || (size_t) g >= groupSlots.size())
        return;

    groupSlots[(size_t) g] = isValidTarget (slotId) ? slotId : kUnassigned;
}

int LearnSession::getHitSlot (int i) const
{
    if (i < 0 || i >= numHits())
        return kUnassigned;

    const size_t idx = (size_t) i;

    if (idx < hitOverrides.size() && hitOverrides[idx] != kUnassigned)
        return hitOverrides[idx];

    if (idx >= clusters.labels.size())
        return kUnassigned;

    const int g = clusters.labels[idx];
    return (g >= 0 && (size_t) g < groupSlots.size()) ? groupSlots[(size_t) g] : kUnassigned;
}

void LearnSession::setHitOverride (int i, int slotId)
{
    if (i < 0 || i >= numHits())
        return;

    hitOverrides.resize (hits.size(), kUnassigned);
    hitOverrides[(size_t) i] = isValidTarget (slotId) ? slotId : kUnassigned;
}

void LearnSession::forgetSlot (int slotId)
{
    if (slotId == kUnassigned)
        return;

    // A cleared override means the hit follows its group again.
    std::replace (groupSlots.begin(), groupSlots.end(), slotId, kUnassigned);
    std::replace (hitOverrides.begin(), hitOverrides.end(), slotId, kUnassigned);
}

std::vector<TrainingHit> LearnSession::makeTrainingHits() const
{
    std::vector<TrainingHit> result;

    for (int i = 0; i < numHits(); ++i)
    {
        const int slot = getHitSlot (i);

        if (slot == kUnassigned || ! isValidTarget (slot))
            continue;

        const DetectedHit& hit = hits[(size_t) i];
        TrainingHit t;
        t.slotId = slot;
        t.sampleRate = sampleRate;
        t.snippet = makeSnippet (audio.data(), (int64_t) audio.size(), hit.onsetSample, sampleRate, t.onsetOffset);
        t.peakDb = hit.peakDb;
        t.features = hit.features;
        result.push_back (std::move (t));
    }

    return result;
}

std::vector<int> LearnSession::slotCounts() const
{
    std::vector<int> counts ((size_t) kMaxSlots + 2, 0);

    for (int i = 0; i < numHits(); ++i)
    {
        const int slot = getHitSlot (i);

        if (slot >= 0 && slot < kMaxSlots)
            ++counts[(size_t) slot];
        else if (slot == kIgnoreSlotId)
            ++counts[(size_t) kMaxSlots];
        else
            ++counts[(size_t) kMaxSlots + 1];
    }

    return counts;
}

void LearnSession::suggest (const std::vector<SlotInfo>& slots, const ClassifierModel* existingModel)
{
    const size_t groupCount = (size_t) std::max (0, clusters.k);
    std::vector<std::vector<Features>> members (groupCount);

    for (size_t i = 0; i < hits.size() && i < clusters.labels.size(); ++i)
    {
        const int g = clusters.labels[i];

        if (g >= 0 && (size_t) g < groupCount)
            members[(size_t) g].push_back (hits[i].features);
    }

    // A model trained with a different analysis window measures different features, so its
    // votes would be meaningless; fall back to the name heuristic.
    const ClassifierModel* model = existingModel;

    if (model != nullptr && std::abs (model->getFeatureSettings().windowMs - featureSettings.windowMs) > 1.0e-4f)
        model = nullptr;

    groupSlots = suggestGroupSlots (members, model, slots);
    groupSlots.resize (groupCount, kUnassigned);
}

} // namespace bbr
