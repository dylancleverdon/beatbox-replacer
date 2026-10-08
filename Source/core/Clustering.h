#pragma once

#include "Classifier.h"
#include "Types.h"

#include <cstdint>
#include <vector>

namespace bbr
{

struct ClusterResult
{
    int k = 0;
    std::vector<int> labels;     // per input point, in [0, k)
    float silhouette = 0.0f;     // mean silhouette score in [-1, 1] (0 when k < 2)
};

// k-means on standardised features (z-score over the given data), k-means++ seeding with a
// deterministic PRNG seeded by `seed`, best of `restarts` runs by inertia, max 100 iterations.
// Output groups are RE-NUMBERED so group 0 has the lowest mean spectral centroid
// (feature kFeatCentroid) and group k-1 the highest -- i.e. roughly kick-like first, hat-like
// last. Deterministic for the same input and seed.
// Edge cases: n == 0 -> k = 0; k is clamped to [1, n].
ClusterResult kmeans (const std::vector<Features>& data, int k, uint32_t seed = 1, int restarts = 8);

// Chooses k in [minK, min(maxK, n - 1)] by silhouette score and returns that clustering.
// If preferredK is in that range and its silhouette is within 0.08 of the best, preferredK wins
// (we usually know how many sounds the user has set up).
// n < 3 -> a single group (k = 1, or k = 0 when empty).
ClusterResult autoCluster (const std::vector<Features>& data, int minK = 2, int maxK = kMaxSlots,
                           int preferredK = 0);

// Suggests a slot for each group.
//   * If `model` is non-null and non-empty: each group gets the slot that wins the majority vote
//     of model->classify() over its members (kIgnoreSlotId allowed).
//   * Otherwise a heuristic on slot names (case-insensitive substring): groups ordered by mean
//     centroid; the lowest-centroid group goes to a slot whose name contains "kick", the highest
//     to one containing "hat", the middle-most to one containing "snare". Every other group
//     stays kUnassigned. A slot is suggested for at most one group in the heuristic mode.
// groupMembers[g] holds the (unstandardised) features of group g's hits.
std::vector<int> suggestGroupSlots (const std::vector<std::vector<Features>>& groupMembers,
                                    const ClassifierModel* model,
                                    const std::vector<SlotInfo>& slots);

} // namespace bbr
