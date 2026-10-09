#include "Clustering.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>

namespace bbr
{

namespace
{
constexpr int kMaxIterations = 100;
constexpr double kStdFloor = 1.0e-3;
constexpr float kPreferredKTolerance = 0.08f;

// Must match the defaults of kmeans() in Clustering.h, so recluster (k) with the k that
// autoCluster() picked reproduces the same grouping.
constexpr uint32_t kDefaultSeed = 1;
constexpr int kDefaultRestarts = 8;

using Point = std::array<double, kNumFeatures>;

// SplitMix64: tiny and bit-identical on every platform (std:: distributions are not).
class Rng
{
public:
    explicit Rng (uint64_t seed) noexcept : state (seed) {}

    uint64_t next() noexcept
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 1).
    double uniform() noexcept { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }

    // Uniform in [0, n); n > 0.
    size_t below (size_t n) noexcept { return std::min ((size_t) (uniform() * (double) n), n - 1); }

private:
    uint64_t state;
};

// z-score per dimension; non-finite inputs are ignored for the statistics and map to 0.
std::vector<Point> standardise (const std::vector<Features>& data)
{
    std::vector<Point> z (data.size());

    for (size_t d = 0; d < (size_t) kNumFeatures; ++d)
    {
        double sum = 0.0;
        size_t count = 0;

        for (const auto& x : data)
        {
            if (std::isfinite (x[d]))
            {
                sum += (double) x[d];
                ++count;
            }
        }

        const double mean = count > 0 ? sum / (double) count : 0.0;
        double sumSq = 0.0;

        for (const auto& x : data)
        {
            if (std::isfinite (x[d]))
            {
                const double diff = (double) x[d] - mean;
                sumSq += diff * diff;
            }
        }

        const double sd = std::max (count > 0 ? std::sqrt (sumSq / (double) count) : 0.0, kStdFloor);

        for (size_t i = 0; i < data.size(); ++i)
        {
            const float v = data[i][d];
            z[i][d] = std::isfinite (v) ? ((double) v - mean) / sd : 0.0;
        }
    }

    return z;
}

double dist2 (const Point& a, const Point& b) noexcept
{
    double s = 0.0;

    for (size_t d = 0; d < a.size(); ++d)
    {
        const double diff = a[d] - b[d];
        s += diff * diff;
    }

    return s;
}

std::vector<Point> seedCentres (const std::vector<Point>& z, size_t k, Rng& rng)
{
    const size_t n = z.size();
    std::vector<Point> centres;
    centres.reserve (k);
    centres.push_back (z[rng.below (n)]);

    std::vector<double> minD2 (n);

    for (size_t i = 0; i < n; ++i)
        minD2[i] = dist2 (z[i], centres[0]);

    while (centres.size() < k)
    {
        double total = 0.0;

        for (double d : minD2)
            total += d;

        size_t pick = 0;

        if (total > 0.0)
        {
            const double target = rng.uniform() * total;
            double acc = 0.0;

            for (size_t i = 0; i < n; ++i)
            {
                if (minD2[i] <= 0.0)
                    continue;

                pick = i; // stays on the last positive-weight point if rounding never crosses target
                acc += minD2[i];

                if (acc > target)
                    break;
            }
        }
        else
        {
            pick = rng.below (n); // every point sits on a centre already
        }

        centres.push_back (z[pick]);

        for (size_t i = 0; i < n; ++i)
            minD2[i] = std::min (minD2[i], dist2 (z[i], z[pick]));
    }

    return centres;
}

struct Run
{
    std::vector<int> labels;
    double inertia = 0.0;
};

// One k-means++ seeding plus Lloyd iterations. Requires 1 <= k <= z.size().
Run runLloyd (const std::vector<Point>& z, size_t k, Rng& rng)
{
    const size_t n = z.size();
    std::vector<Point> centres = seedCentres (z, k, rng);
    std::vector<Point> sums (k);
    std::vector<size_t> counts (k);
    std::vector<double> ownDist (n, 0.0);

    Run run;
    run.labels.assign (n, -1);

    for (int iter = 0; iter < kMaxIterations; ++iter)
    {
        bool changed = false;

        for (size_t i = 0; i < n; ++i)
        {
            size_t best = 0;
            double bestD = dist2 (z[i], centres[0]);

            for (size_t c = 1; c < k; ++c)
            {
                const double d = dist2 (z[i], centres[c]);

                if (d < bestD)
                {
                    bestD = d;
                    best = c;
                }
            }

            ownDist[i] = bestD;

            if (run.labels[i] != (int) best)
            {
                run.labels[i] = (int) best;
                changed = true;
            }
        }

        std::fill (counts.begin(), counts.end(), (size_t) 0);

        for (int label : run.labels)
            ++counts[(size_t) label];

        // An empty cluster takes the point farthest from its centre (from a cluster that can spare one).
        for (size_t c = 0; c < k; ++c)
        {
            if (counts[c] != 0)
                continue;

            size_t far = n;
            double farD = -1.0;

            for (size_t i = 0; i < n; ++i)
            {
                if (counts[(size_t) run.labels[i]] > 1 && ownDist[i] > farD)
                {
                    farD = ownDist[i];
                    far = i;
                }
            }

            if (far == n)
                break;

            --counts[(size_t) run.labels[far]];
            run.labels[far] = (int) c;
            counts[c] = 1;
            ownDist[far] = 0.0;
            changed = true;
        }

        for (auto& s : sums)
            s.fill (0.0);

        for (size_t i = 0; i < n; ++i)
        {
            auto& s = sums[(size_t) run.labels[i]];

            for (size_t d = 0; d < s.size(); ++d)
                s[d] += z[i][d];
        }

        for (size_t c = 0; c < k; ++c)
            if (counts[c] > 0)
                for (size_t d = 0; d < centres[c].size(); ++d)
                    centres[c][d] = sums[c][d] / (double) counts[c];

        if (! changed)
            break;
    }

    for (size_t i = 0; i < n; ++i)
        run.inertia += dist2 (z[i], centres[(size_t) run.labels[i]]);

    return run;
}

// Renumbers labels so group 0 has the lowest mean raw spectral centroid. Empty groups go last.
void renumberByCentroid (const std::vector<Features>& data, std::vector<int>& labels, int k)
{
    const size_t numGroups = (size_t) k;
    std::vector<double> sum (numGroups, 0.0);
    std::vector<size_t> count (numGroups, 0);

    for (size_t i = 0; i < labels.size(); ++i)
    {
        const float v = data[i][(size_t) kFeatCentroid];

        if (std::isfinite (v))
        {
            sum[(size_t) labels[i]] += (double) v;
            ++count[(size_t) labels[i]];
        }
    }

    std::vector<double> meanCentroid (numGroups);

    for (size_t g = 0; g < numGroups; ++g)
        meanCentroid[g] = count[g] > 0 ? sum[g] / (double) count[g]
                                       : std::numeric_limits<double>::infinity();

    std::vector<int> order (numGroups);
    std::iota (order.begin(), order.end(), 0);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b)
    {
        return meanCentroid[(size_t) a] < meanCentroid[(size_t) b];
    });

    std::vector<int> newLabel (numGroups);

    for (size_t rank = 0; rank < numGroups; ++rank)
        newLabel[(size_t) order[rank]] = (int) rank;

    for (auto& label : labels)
        label = newLabel[(size_t) label];
}

// Clustering without the silhouette (callers add it). data must be non-empty.
ClusterResult clusterStandardised (const std::vector<Features>& data, const std::vector<Point>& z,
                                   int k, uint32_t seed, int restarts)
{
    ClusterResult result;
    result.k = std::clamp (k, 1, (int) z.size());

    if (result.k == 1)
    {
        result.labels.assign (z.size(), 0);
        return result;
    }

    Run best;
    bool haveBest = false;

    for (int r = 0; r < std::max (1, restarts); ++r)
    {
        Rng rng (((uint64_t) seed << 32) | (uint64_t) (uint32_t) r);
        Run run = runLloyd (z, (size_t) result.k, rng);

        if (! haveBest || run.inertia < best.inertia)
        {
            best = std::move (run);
            haveBest = true;
        }
    }

    result.labels = std::move (best.labels);
    renumberByCentroid (data, result.labels, result.k);
    return result;
}

// Euclidean distances for all pairs i < j, row by row.
std::vector<float> pairwiseDistances (const std::vector<Point>& z)
{
    const size_t n = z.size();
    std::vector<float> dist;
    dist.reserve (n > 1 ? n * (n - 1) / 2 : 0);

    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            dist.push_back ((float) std::sqrt (dist2 (z[i], z[j])));

    return dist;
}

// Mean silhouette; points in singleton clusters score 0.
float silhouetteScore (const std::vector<float>& dist, const std::vector<int>& labels, int k)
{
    const size_t n = labels.size();
    const size_t numGroups = (size_t) std::max (0, k);

    if (numGroups < 2 || n < 2)
        return 0.0f;

    std::vector<size_t> count (numGroups, 0);

    for (int label : labels)
        ++count[(size_t) label];

    // sums[i * numGroups + g] = total distance from point i to the points of group g
    std::vector<double> sums (n * numGroups, 0.0);
    size_t idx = 0;

    for (size_t i = 0; i < n; ++i)
    {
        for (size_t j = i + 1; j < n; ++j, ++idx)
        {
            const double d = (double) dist[idx];
            sums[i * numGroups + (size_t) labels[j]] += d;
            sums[j * numGroups + (size_t) labels[i]] += d;
        }
    }

    double total = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        const size_t own = (size_t) labels[i];

        if (count[own] < 2)
            continue;

        const double a = sums[i * numGroups + own] / (double) (count[own] - 1);
        double b = std::numeric_limits<double>::infinity();

        for (size_t g = 0; g < numGroups; ++g)
            if (g != own && count[g] > 0)
                b = std::min (b, sums[i * numGroups + g] / (double) count[g]);

        const double denom = std::max (a, b);

        if (std::isfinite (b) && denom > 0.0)
            total += (b - a) / denom;
    }

    return (float) (total / (double) n);
}

std::string toLowerAscii (std::string s)
{
    for (auto& ch : s)
        ch = (char) std::tolower ((unsigned char) ch);

    return s;
}

bool containsSlot (const std::vector<SlotInfo>& slots, int slotId)
{
    return std::any_of (slots.begin(), slots.end(), [slotId] (const SlotInfo& s) { return s.id == slotId; });
}
} // namespace

ClusterResult kmeans (const std::vector<Features>& data, int k, uint32_t seed, int restarts)
{
    if (data.empty())
        return {};

    const auto z = standardise (data);
    auto result = clusterStandardised (data, z, k, seed, restarts);

    if (result.k >= 2)
        result.silhouette = silhouetteScore (pairwiseDistances (z), result.labels, result.k);

    return result;
}

ClusterResult autoCluster (const std::vector<Features>& data, int minK, int maxK, int preferredK)
{
    const int n = (int) data.size();

    if (n < 3)
    {
        ClusterResult result;
        result.k = n == 0 ? 0 : 1;
        result.labels.assign ((size_t) n, 0);
        return result;
    }

    const int lo = std::max (1, minK);
    const int hi = std::min (maxK, n - 1);

    if (hi < lo)
        return kmeans (data, std::max (1, hi), kDefaultSeed, kDefaultRestarts);

    const auto z = standardise (data);
    const auto dist = pairwiseDistances (z);

    ClusterResult best, preferred;
    bool haveBest = false, havePreferred = false;

    for (int k = lo; k <= hi; ++k)
    {
        auto candidate = clusterStandardised (data, z, k, kDefaultSeed, kDefaultRestarts);
        candidate.silhouette = candidate.k >= 2 ? silhouetteScore (dist, candidate.labels, candidate.k) : 0.0f;

        if (k == preferredK)
        {
            preferred = candidate;
            havePreferred = true;
        }

        if (! haveBest || candidate.silhouette > best.silhouette)
        {
            best = std::move (candidate);
            haveBest = true;
        }
    }

    if (havePreferred && preferred.silhouette >= best.silhouette - kPreferredKTolerance)
        return preferred;

    return best;
}

std::vector<int> suggestGroupSlots (const std::vector<std::vector<Features>>& groupMembers,
                                    const ClassifierModel* model,
                                    const std::vector<SlotInfo>& slots)
{
    const size_t numGroups = groupMembers.size();
    std::vector<int> result (numGroups, kUnassigned);

    if (model != nullptr && ! model->empty())
    {
        for (size_t g = 0; g < numGroups; ++g)
        {
            // [0, kMaxSlots) slots, [kMaxSlots] ignore. Ties go to the higher summed confidence.
            int votes[kMaxSlots + 1] {};
            float confidence[kMaxSlots + 1] {};

            for (const auto& f : groupMembers[g])
            {
                const auto c = model->classify (f);
                int idx = -1;

                if (c.slotId == kIgnoreSlotId)
                    idx = kMaxSlots;
                else if (c.slotId >= 0 && c.slotId < kMaxSlots && (slots.empty() || containsSlot (slots, c.slotId)))
                    idx = c.slotId; // a slot the user has since deleted is not suggested

                if (idx >= 0)
                {
                    ++votes[idx];
                    confidence[idx] += c.confidence;
                }
            }

            int winner = -1;

            for (int i = 0; i <= kMaxSlots; ++i)
            {
                if (votes[i] == 0)
                    continue;

                if (winner < 0 || votes[i] > votes[winner]
                    || (votes[i] == votes[winner] && confidence[i] > confidence[winner]))
                    winner = i;
            }

            if (winner >= 0)
                result[g] = winner == kMaxSlots ? kIgnoreSlotId : winner;
        }

        return result;
    }

    // Heuristic: order the (non-empty) groups by mean spectral centroid.
    struct GroupCentroid
    {
        size_t group;
        double centroid;
    };

    std::vector<GroupCentroid> ordered;

    for (size_t g = 0; g < numGroups; ++g)
    {
        double sum = 0.0;
        size_t count = 0;

        for (const auto& f : groupMembers[g])
        {
            const float v = f[(size_t) kFeatCentroid];

            if (std::isfinite (v))
            {
                sum += (double) v;
                ++count;
            }
        }

        if (count > 0)
            ordered.push_back ({ g, sum / (double) count });
    }

    // One group alone could be anything; leave it for the user.
    if (ordered.size() < 2)
        return result;

    std::stable_sort (ordered.begin(), ordered.end(), [] (const GroupCentroid& a, const GroupCentroid& b)
    {
        return a.centroid < b.centroid;
    });

    std::vector<int> used;

    auto assignByName = [&] (size_t group, const char* keyword)
    {
        for (const auto& s : slots)
        {
            if (s.id < 0 || s.id >= kMaxSlots || std::find (used.begin(), used.end(), s.id) != used.end())
                continue;

            if (toLowerAscii (s.name).find (keyword) != std::string::npos)
            {
                result[group] = s.id;
                used.push_back (s.id);
                return;
            }
        }
    };

    const size_t m = ordered.size();
    assignByName (ordered.front().group, "kick");
    assignByName (ordered.back().group, "hat");

    if (m >= 3)
        assignByName (ordered[(m - 1) / 2].group, "snare");

    return result;
}

} // namespace bbr
