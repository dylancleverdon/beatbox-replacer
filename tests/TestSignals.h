#pragma once

// Synthetic beatbox-like test signals for the core unit tests.
// Everything is deterministic: the same seed always gives the same samples.

#include <cstdint>
#include <vector>

namespace bbrtest
{

constexpr double kPi = 3.14159265358979323846;

// Small deterministic PRNG (splitmix64), identical on every platform/compiler.
class Rng
{
public:
    explicit Rng (uint64_t seed) noexcept : state (seed) {}

    uint64_t nextU64() noexcept;
    double uniform() noexcept;                       // [0, 1)
    double uniform (double lo, double hi) noexcept;  // [lo, hi)
    int uniformInt (int lo, int hi) noexcept;        // [lo, hi]
    double gaussian() noexcept;                      // N(0, 1)

private:
    uint64_t state;
    bool hasSpare = false;
    double spare = 0.0;
};

enum class Sound
{
    kick,    // "b"
    snare,   // "pf" / "k"
    hat,     // "ts"
    click,   // tongue/mouth click: very short, should be trained as "ignore"
    breath   // slow broadband swell, should (mostly) not trigger at all
};

const char* soundName (Sound s) noexcept;

// Default peak amplitude of each sound in patterns (hats quieter than kicks, like real beatboxing).
float defaultAmplitude (Sound s) noexcept;

// Per-instance variation, so no two hits are identical.
struct Variation
{
    double gainDb = 0.0;   // applied on top of the requested amplitude
    double pitch = 1.0;    // multiplies every frequency (tones and filter corners)
    double decay = 1.0;    // multiplies every decay time

    static Variation random (Rng& rng) noexcept; // +-3 dB, +-10 % pitch, +-15 % decay
};

// One sound starting at sample 0, including its whole decay tail. The result is scaled so its
// peak |x| equals amplitude * dB(v.gainDb). Noise is generated with `rng`; its spectral density
// does not depend on the sample rate.
std::vector<float> synthesize (Sound s, double sampleRate, float amplitude, Rng& rng,
                               const Variation& v = {});

// Deterministic broadband "noise" built from sinusoids with seeded random frequencies and phases,
// evaluated in continuous time: the same seed gives the same waveform at any sample rate.
// Decays with time constant decayMs after a 0.5 ms attack. Peak normalised to `amplitude`.
std::vector<float> synthesizeRateIndependent (double sampleRate, double lengthSec, double loHz,
                                              double hiHz, int numPartials, double decayMs,
                                              float amplitude, uint64_t seed);

// Gaussian white noise with the given RMS level in dBFS.
std::vector<float> whiteNoise (int64_t numSamples, float rmsDb, uint64_t seed);

// ---------------------------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------------------------
struct PatternEvent
{
    double timeSec = 0.0;
    Sound sound = Sound::kick;
    float amplitude = -1.0f;   // < 0: defaultAmplitude(sound)
};

struct TruthHit
{
    double timeSec = 0.0;
    int64_t sample = 0;        // first sample of the sound in the pattern buffer
    Sound sound = Sound::kick;
    float peak = 0.0f;         // peak |x| of the sound alone (after variation)
};

struct PatternOptions
{
    float noiseDb = -70.0f;    // background noise RMS, dBFS (<= -200 for none)
    double tailSec = 0.4;      // silence (noise) kept after the end of the last sound
    bool vary = true;          // apply Variation::random to every event
    float gainDb = 0.0f;       // extra gain applied to every event
};

struct Pattern
{
    double sampleRate = 48000.0;
    std::vector<float> audio;
    std::vector<TruthHit> truth; // sorted by time
};

// Places the events (times in seconds) into a buffer with background noise.
Pattern buildPattern (const std::vector<PatternEvent>& events, double sampleRate, uint64_t seed,
                      const PatternOptions& options = {});

// A shuffled sequence with the given number of each sound, starting at 0.2 s, with onset-to-onset
// gaps drawn uniformly from [minGapSec, maxGapSec].
Pattern makeMixedPattern (double sampleRate, uint64_t seed, int kicks, int snares, int hats,
                          int clicks = 0, double minGapSec = 0.15, double maxGapSec = 0.3,
                          const PatternOptions& options = {});

} // namespace bbrtest
