#include "Classifier.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bbr
{

namespace
{
constexpr int kMaxNeighbours = 16;

// Index into the per-class arrays: a slot id maps to itself, kIgnoreSlotId to kMaxSlots,
// anything else (including kUnassigned) to -1.
int classIndex (int slotId) noexcept
{
    if (slotId == kIgnoreSlotId)
        return kMaxSlots;

    return (slotId >= 0 && slotId < kMaxSlots) ? slotId : -1;
}

bool allFinite (const Features& f) noexcept
{
    for (float v : f)
        if (! std::isfinite (v))
            return false;

    return true;
}

float medianOf (std::vector<float>& values)
{
    if (values.empty())
        return std::numeric_limits<float>::quiet_NaN();

    std::sort (values.begin(), values.end());
    const size_t mid = values.size() / 2;

    if (values.size() % 2 != 0)
        return values[mid];

    return 0.5f * (values[mid - 1] + values[mid]);
}
} // namespace

std::shared_ptr<const ClassifierModel> ClassifierModel::build (const std::vector<TrainingHit>& hits,
                                                               const FeatureSettings& settings,
                                                               int numNeighbours)
{
    auto model = std::make_shared<ClassifierModel>();
    model->featureSettings = settings;
    model->k = std::clamp (numNeighbours, 1, kMaxNeighbours);

    for (auto& m : model->medianPeak)
        m = std::numeric_limits<float>::quiet_NaN();

    // kUnassigned, out-of-range ids and broken feature vectors never enter the model.
    std::vector<const TrainingHit*> used;
    used.reserve (hits.size());

    for (const auto& h : hits)
        if (classIndex (h.slotId) >= 0 && allFinite (h.features))
            used.push_back (&h);

    if (used.empty())
        return model;

    const double n = (double) used.size();

    for (size_t d = 0; d < (size_t) kNumFeatures; ++d)
    {
        double sum = 0.0;

        for (const auto* h : used)
            sum += (double) h->features[d];

        const double m = sum / n;
        double sumSq = 0.0;

        for (const auto* h : used)
        {
            const double diff = (double) h->features[d] - m;
            sumSq += diff * diff;
        }

        const double sd = std::max (std::sqrt (sumSq / n), 1.0e-3);
        model->mean[d] = (float) m;
        model->invStd[d] = (float) (1.0 / sd);
    }

    model->points.reserve (used.size());
    model->labels.reserve (used.size());
    std::vector<float> peaks[kMaxSlots];

    for (const auto* h : used)
    {
        const int c = classIndex (h->slotId);

        if (c < 0)
            continue; // filtered above; keeps the compiler's bounds analysis happy

        // Same float arithmetic as classify(), so a training point classifies at distance 0.
        Features z {};

        for (size_t d = 0; d < z.size(); ++d)
            z[d] = (h->features[d] - model->mean[d]) * model->invStd[d];

        model->points.push_back (z);
        model->labels.push_back (h->slotId);
        ++model->counts[c];

        if (c < kMaxSlots && std::isfinite (h->peakDb))
            peaks[c].push_back (h->peakDb);
    }

    for (int s = 0; s < kMaxSlots; ++s)
        model->medianPeak[s] = medianOf (peaks[s]);

    return model;
}

Classification ClassifierModel::classify (const Features& f) const noexcept
{
    Classification result;
    const size_t n = std::min (labels.size(), points.size());

    if (n == 0)
        return result;

    Features z {};

    for (size_t d = 0; d < z.size(); ++d)
    {
        const float v = (f[d] - mean[d]) * invStd[d];
        z[d] = std::isfinite (v) ? v : 0.0f;
    }

    // The k nearest points so far, sorted by squared distance (insertion keeps it allocation-free).
    const size_t maxFound = std::min ((size_t) std::clamp (k, 1, kMaxNeighbours), n);
    float nearestDist[kMaxNeighbours] {};
    size_t nearestIndex[kMaxNeighbours] {};
    size_t found = 0;

    for (size_t i = 0; i < n; ++i)
    {
        const Features& p = points[i];
        float d2 = 0.0f;

        for (size_t d = 0; d < z.size(); ++d)
        {
            const float diff = z[d] - p[d];
            d2 += diff * diff;
        }

        if (found == maxFound && ! (d2 < nearestDist[maxFound - 1]))
            continue;

        size_t pos = found < maxFound ? found++ : maxFound - 1;

        while (pos > 0 && nearestDist[pos - 1] > d2)
        {
            nearestDist[pos] = nearestDist[pos - 1];
            nearestIndex[pos] = nearestIndex[pos - 1];
            --pos;
        }

        nearestDist[pos] = d2;
        nearestIndex[pos] = i;
    }

    // Classes are recorded in order of their nearest neighbour, so a strict ">" below hands
    // ties to the class with the nearest point.
    float weights[kMaxSlots + 1] {};
    bool seen[kMaxSlots + 1] {};
    int order[kMaxSlots + 1] {};
    int numSeen = 0;
    float total = 0.0f;

    for (size_t j = 0; j < found; ++j)
    {
        const int c = classIndex (labels[nearestIndex[j]]);

        if (c < 0)
            continue;

        const float w = 1.0f / (std::sqrt (nearestDist[j]) + 1.0e-6f);

        if (! seen[c])
        {
            seen[c] = true;
            order[numSeen++] = c;
        }

        weights[c] += w;
        total += w;
    }

    if (numSeen == 0)
        return result;

    int winner = order[0];

    for (int i = 1; i < numSeen; ++i)
        if (weights[order[i]] > weights[winner])
            winner = order[i];

    result.slotId = winner == kMaxSlots ? kIgnoreSlotId : winner;
    result.confidence = total > 0.0f ? std::clamp (weights[winner] / total, 0.0f, 1.0f) : 0.0f;
    return result;
}

float ClassifierModel::getMedianPeakDb (int slotId) const noexcept
{
    if (slotId < 0 || slotId >= kMaxSlots || counts[slotId] == 0)
        return std::numeric_limits<float>::quiet_NaN();

    return medianPeak[slotId];
}

int ClassifierModel::countFor (int slotId) const noexcept
{
    const int c = classIndex (slotId);
    return c >= 0 ? counts[c] : 0;
}

} // namespace bbr
