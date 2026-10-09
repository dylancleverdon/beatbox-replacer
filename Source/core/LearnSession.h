#pragma once

#include "Classifier.h"
#include "Clustering.h"
#include "Types.h"

#include <vector>

namespace bbr
{

// One "Learn" pass: a recording of the user's beatboxing, the hits found in it, the automatic
// grouping of similar hits, and the user's decisions about which slot each group / hit maps to.
// Pure data + logic; owned and used only on the message thread.
class LearnSession
{
public:
    // Replaces the session with a new recording and clears hits/groups/assignments.
    void setAudio (std::vector<float> mono, double sampleRate);

    // Detects hits, auto-clusters them (preferredK = number of slots the user has) and fills in
    // suggested slots via suggestGroupSlots(existingModel, slots). existingModel may be null.
    void analyze (const DetectorSettings& detector, const FeatureSettings& features,
                  const std::vector<SlotInfo>& slots, const ClassifierModel* existingModel);

    // Re-runs k-means with a user-chosen number of groups (clamped to [1, numHits()]), keeps
    // per-hit overrides and re-suggests group slots.
    void recluster (int k, const std::vector<SlotInfo>& slots, const ClassifierModel* existingModel);

    void clear();

    bool hasAudio() const noexcept { return ! audio.empty(); }
    const std::vector<float>& getAudio() const noexcept { return audio; }
    double getSampleRate() const noexcept { return sampleRate; }
    const FeatureSettings& getFeatureSettings() const noexcept { return featureSettings; }

    int numHits() const noexcept { return (int) hits.size(); }
    const DetectedHit& getHit (int i) const { return hits[(size_t) i]; }

    int numGroups() const noexcept { return clusters.k; }
    int groupOfHit (int i) const { return clusters.labels[(size_t) i]; }
    int groupSize (int g) const;
    // Index of the hit closest to the group's centre in feature space (for auditioning), or -1.
    int representativeHit (int g) const;

    int getGroupSlot (int g) const { return groupSlots[(size_t) g]; }
    void setGroupSlot (int g, int slotId);

    // A hit's effective slot: its override if set, otherwise its group's slot.
    int getHitSlot (int i) const;
    bool hasHitOverride (int i) const { return hitOverrides[(size_t) i] != kUnassigned; }
    // Pass kUnassigned to remove the override (hit follows its group again).
    void setHitOverride (int i, int slotId);

    // Adds a hit at onsetSample (clamped to the recording), measured like a detected one. It joins
    // the group whose centre is nearest in feature space (group 0 if there were no groups) and
    // follows that group's sound. Hits stay sorted by onset. Returns the new hit's index, or -1
    // if there is no audio or a hit already sits at that sample.
    int addHit (int64_t onsetSample);
    // Removes a hit. A group left without hits is removed and later groups are renumbered.
    void removeHit (int i);

    // When a slot is deleted: every group/hit pointing at it becomes kUnassigned.
    void forgetSlot (int slotId);

    // Training examples for every hit whose effective slot is a slot id or kIgnoreSlotId
    // (kUnassigned hits are skipped). Snippets are cut from the recording with makeSnippet().
    std::vector<TrainingHit> makeTrainingHits() const;

    // Effective-slot counts: index [0, kMaxSlots) per slot, [kMaxSlots] = ignore,
    // [kMaxSlots + 1] = unassigned.
    std::vector<int> slotCounts() const;

private:
    std::vector<float> audio;
    double sampleRate = 48000.0;
    FeatureSettings featureSettings;
    std::vector<DetectedHit> hits;
    ClusterResult clusters;
    std::vector<int> groupSlots;
    std::vector<int> hitOverrides;

    void suggest (const std::vector<SlotInfo>& slots, const ClassifierModel* existingModel);
};

} // namespace bbr
