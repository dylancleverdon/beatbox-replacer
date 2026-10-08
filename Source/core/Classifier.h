#pragma once

#include "Types.h"

#include <memory>
#include <vector>

namespace bbr
{

struct Classification
{
    int slotId = kUnassigned;  // a slot id, kIgnoreSlotId, or kUnassigned if the model is empty
    float confidence = 0.0f;   // 0..1: share of the distance-weighted vote won by slotId
};

// Immutable k-nearest-neighbour model built from labelled training hits.
//
// build(): features are standardised (z-score per dimension, std floored at 1e-3) using the
// training set's mean/std; classify() standardises its input the same way and takes a
// distance-weighted vote (weight 1 / (euclidean distance + 1e-6)) among the k nearest
// training points (k = min(k, number of points)). Ties go to the slot with the nearest point.
// Hits labelled kUnassigned are skipped; kIgnoreSlotId is a normal class.
//
// Once built, a model is never modified, so the audio thread can use it while the message
// thread builds a replacement.
class ClassifierModel
{
public:
    static std::shared_ptr<const ClassifierModel> build (const std::vector<TrainingHit>& hits,
                                                         const FeatureSettings& settings,
                                                         int k = 5);

    // Real-time safe: no allocation, no locks.
    Classification classify (const Features& f) const noexcept;

    bool empty() const noexcept { return labels.empty(); }
    int size() const noexcept { return (int) labels.size(); }

    // Settings the training features were computed with. Live analysis must use the same
    // settings for its features to be comparable.
    const FeatureSettings& getFeatureSettings() const noexcept { return featureSettings; }

    // Median training peakDb of a slot (for dynamic velocity); NaN if the slot has no hits.
    float getMedianPeakDb (int slotId) const noexcept;

    // Number of training points labelled with slotId.
    int countFor (int slotId) const noexcept;

private:
    FeatureSettings featureSettings;
    int k = 5;
    Features mean {}, invStd {};
    std::vector<Features> points; // standardised
    std::vector<int> labels;
    float medianPeak[kMaxSlots] {};
    int counts[kMaxSlots + 1] {}; // [kMaxSlots] = ignore class
};

} // namespace bbr
