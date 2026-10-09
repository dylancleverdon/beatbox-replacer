#include "TestSignals.h"

#include <algorithm>
#include <cmath>

namespace bbrtest
{

// ---------------------------------------------------------------------------------------------
// Rng
// ---------------------------------------------------------------------------------------------
uint64_t Rng::nextU64() noexcept
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double Rng::uniform() noexcept
{
    return (double) (nextU64() >> 11) * (1.0 / 9007199254740992.0);
}

double Rng::uniform (double lo, double hi) noexcept
{
    return lo + (hi - lo) * uniform();
}

int Rng::uniformInt (int lo, int hi) noexcept
{
    const auto range = (uint64_t) ((int64_t) hi - (int64_t) lo + 1);
    return (int) ((int64_t) lo + (int64_t) (nextU64() % range));
}

double Rng::gaussian() noexcept
{
    if (hasSpare)
    {
        hasSpare = false;
        return spare;
    }

    // Box-Muller
    double u1 = uniform();
    while (u1 <= 1.0e-300)
        u1 = uniform();

    const double u2 = uniform();
    const double r = std::sqrt (-2.0 * std::log (u1));
    spare = r * std::sin (2.0 * kPi * u2);
    hasSpare = true;
    return r * std::cos (2.0 * kPi * u2);
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
namespace
{
    // RBJ cookbook biquad, double precision.
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        static Biquad make (double sampleRate, double freq, double q, bool highPass)
        {
            freq = std::min (freq, 0.45 * sampleRate);
            const double w0 = 2.0 * kPi * freq / sampleRate;
            const double cosw = std::cos (w0);
            const double alpha = std::sin (w0) / (2.0 * q);
            const double a0 = 1.0 + alpha;

            Biquad f;
            if (highPass)
            {
                f.b0 = (1.0 + cosw) * 0.5 / a0;
                f.b1 = -(1.0 + cosw) / a0;
            }
            else
            {
                f.b0 = (1.0 - cosw) * 0.5 / a0;
                f.b1 = (1.0 - cosw) / a0;
            }
            f.b2 = f.b0;
            f.a1 = -2.0 * cosw / a0;
            f.a2 = (1.0 - alpha) / a0;
            return f;
        }

        static Biquad lowPass (double sampleRate, double freq, double q = 0.7071)  { return make (sampleRate, freq, q, false); }
        static Biquad highPass (double sampleRate, double freq, double q = 0.7071) { return make (sampleRate, freq, q, true); }

        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

    int lengthFor (double sampleRate, double seconds) noexcept
    {
        return std::max (1, (int) std::ceil (sampleRate * seconds));
    }

    // Raised-cosine attack from 0 to 1 over attackSec.
    double attack (double t, double attackSec) noexcept
    {
        if (t >= attackSec)
            return 1.0;

        return 0.5 - 0.5 * std::cos (kPi * t / attackSec);
    }

    // White noise whose spectral density does not depend on the sample rate.
    double noiseSample (Rng& rng, double sampleRate) noexcept
    {
        return rng.gaussian() * std::sqrt (sampleRate / 48000.0);
    }

    std::vector<float> normalisePeak (const std::vector<double>& y, double peak)
    {
        double maxAbs = 0.0;
        for (auto v : y)
            maxAbs = std::max (maxAbs, std::abs (v));

        const double scale = maxAbs > 0.0 ? peak / maxAbs : 0.0;
        std::vector<float> out (y.size());
        for (size_t i = 0; i < y.size(); ++i)
            out[i] = (float) (y[i] * scale);

        return out;
    }

    // "b": sine sweeping 150 -> 50 Hz over 40 ms, ~80 ms body, plus a short low-passed noise click.
    std::vector<double> makeKick (double sr, Rng& rng, const Variation& v)
    {
        const double tau = 0.024 * v.decay;
        const double f0 = 150.0 * v.pitch, f1 = 50.0 * v.pitch;
        const int n = lengthFor (sr, 0.002 + 9.0 * tau);
        auto click = Biquad::lowPass (sr, 2500.0 * v.pitch);

        std::vector<double> y ((size_t) n);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double sweep = std::max (0.0, 1.0 - t / 0.040);
            const double f = f1 * std::pow (f0 / f1, sweep);
            const double body = std::sin (phase) * attack (t, 0.001) * std::exp (-t / tau);
            phase += 2.0 * kPi * f / sr;

            const double noise = t < 0.012 ? noiseSample (rng, sr) : 0.0;
            y[(size_t) i] = body + 0.35 * click.process (noise) * std::exp (-t / 0.0015);
        }
        return y;
    }

    // "pf"/"k": noise band-passed to ~1-4 kHz with ~60 ms decay, plus a ~200 Hz tone.
    std::vector<double> makeSnare (double sr, Rng& rng, const Variation& v)
    {
        const double tauNoise = 0.018 * v.decay, tauTone = 0.028 * v.decay;
        const double toneHz = 200.0 * v.pitch;
        const int n = lengthFor (sr, 0.001 + 9.0 * tauTone);
        auto hp = Biquad::highPass (sr, 1000.0 * v.pitch);
        auto lp = Biquad::lowPass (sr, 4000.0 * v.pitch);

        std::vector<double> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double noise = lp.process (hp.process (noiseSample (rng, sr)));
            const double tone = std::sin (2.0 * kPi * toneHz * t);
            y[(size_t) i] = attack (t, 0.0005) * (noise * std::exp (-t / tauNoise)
                                                  + 0.25 * tone * std::exp (-t / tauTone));
        }
        return y;
    }

    // "ts": noise high-passed (4th order) above ~7 kHz, ~35 ms decay.
    std::vector<double> makeHat (double sr, Rng& rng, const Variation& v)
    {
        const double tau = 0.009 * v.decay;
        const int n = lengthFor (sr, 0.001 + 9.0 * tau);
        auto hp1 = Biquad::highPass (sr, 7000.0 * v.pitch);
        auto hp2 = Biquad::highPass (sr, 7000.0 * v.pitch);

        std::vector<double> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double noise = hp2.process (hp1.process (noiseSample (rng, sr)));
            y[(size_t) i] = noise * attack (t, 0.0002) * std::exp (-t / tau);
        }
        return y;
    }

    // Mouth click: a ~1.8 kHz ping lasting a few milliseconds.
    std::vector<double> makeClick (double sr, Rng& rng, const Variation& v)
    {
        const double tau = 0.0008 * v.decay;
        const double f = 1800.0 * v.pitch;
        const int n = lengthFor (sr, 0.015);
        auto hp = Biquad::highPass (sr, 1000.0 * v.pitch);
        auto lp = Biquad::lowPass (sr, 5000.0 * v.pitch);

        std::vector<double> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double noise = lp.process (hp.process (t < 0.004 ? noiseSample (rng, sr) : 0.0));
            y[(size_t) i] = std::sin (2.0 * kPi * f * t) * attack (t, 0.0001) * std::exp (-t / tau)
                          + 0.4 * noise * std::exp (-t / 0.0004);
        }
        return y;
    }

    // Breath: band-limited noise whose level ramps linearly in dB (60 dB over 400 ms), so the
    // level never rises by anything near riseDb within 30 ms.
    std::vector<double> makeBreath (double sr, Rng& rng, const Variation& v)
    {
        const double rise = 0.4 * v.decay, hold = 0.15, release = 0.3 * v.decay;
        const int n = lengthFor (sr, rise + hold + release);
        auto lp = Biquad::lowPass (sr, 3000.0 * v.pitch);
        auto hp = Biquad::highPass (sr, 300.0 * v.pitch);

        std::vector<double> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            double envDb = 0.0;
            if (t < rise)
                envDb = -60.0 + 60.0 * t / rise;
            else if (t > rise + hold)
                envDb = -60.0 * (t - rise - hold) / release;

            y[(size_t) i] = lp.process (hp.process (noiseSample (rng, sr))) * dbToGain (envDb);
        }
        return y;
    }
}

// ---------------------------------------------------------------------------------------------
const char* soundName (Sound s) noexcept
{
    switch (s)
    {
        case Sound::kick:   return "kick";
        case Sound::snare:  return "snare";
        case Sound::hat:    return "hat";
        case Sound::click:  return "click";
        case Sound::breath: return "breath";
    }
    return "?";
}

float defaultAmplitude (Sound s) noexcept
{
    switch (s)
    {
        case Sound::kick:   return 0.5f;
        case Sound::snare:  return 0.4f;
        case Sound::hat:    return 0.15f;
        case Sound::click:  return 0.1f;
        case Sound::breath: return 0.05f;
    }
    return 0.1f;
}

Variation Variation::random (Rng& rng) noexcept
{
    Variation v;
    v.gainDb = rng.uniform (-3.0, 3.0);
    v.pitch = rng.uniform (0.9, 1.1);
    v.decay = rng.uniform (0.85, 1.15);
    return v;
}

std::vector<float> synthesize (Sound s, double sampleRate, float amplitude, Rng& rng, const Variation& v)
{
    std::vector<double> y;
    switch (s)
    {
        case Sound::kick:   y = makeKick (sampleRate, rng, v); break;
        case Sound::snare:  y = makeSnare (sampleRate, rng, v); break;
        case Sound::hat:    y = makeHat (sampleRate, rng, v); break;
        case Sound::click:  y = makeClick (sampleRate, rng, v); break;
        case Sound::breath: y = makeBreath (sampleRate, rng, v); break;
    }
    return normalisePeak (y, (double) amplitude * dbToGain (v.gainDb));
}

std::vector<float> synthesizeRateIndependent (double sampleRate, double lengthSec, double loHz,
                                              double hiHz, int numPartials, double decayMs,
                                              float amplitude, uint64_t seed)
{
    Rng rng (seed);
    std::vector<double> freqs ((size_t) numPartials), phases ((size_t) numPartials);
    for (int p = 0; p < numPartials; ++p)
    {
        freqs[(size_t) p] = std::exp (rng.uniform (std::log (loHz), std::log (hiHz)));
        phases[(size_t) p] = rng.uniform (0.0, 2.0 * kPi);
    }

    const int n = lengthFor (sampleRate, lengthSec);
    const double tau = decayMs * 0.001;
    std::vector<double> y ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sampleRate;
        double sum = 0.0;
        for (int p = 0; p < numPartials; ++p)
            sum += std::sin (2.0 * kPi * freqs[(size_t) p] * t + phases[(size_t) p]);

        y[(size_t) i] = sum * attack (t, 0.0005) * std::exp (-t / tau);
    }
    return normalisePeak (y, amplitude);
}

std::vector<float> whiteNoise (int64_t numSamples, float rmsDb, uint64_t seed)
{
    Rng rng (seed);
    const double g = dbToGain (rmsDb);
    std::vector<float> out ((size_t) std::max<int64_t> (0, numSamples));
    for (auto& x : out)
        x = (float) (rng.gaussian() * g);

    return out;
}

// ---------------------------------------------------------------------------------------------
Pattern buildPattern (const std::vector<PatternEvent>& eventsIn, double sampleRate, uint64_t seed,
                      const PatternOptions& options)
{
    auto events = eventsIn;
    std::stable_sort (events.begin(), events.end(),
                      [] (const PatternEvent& a, const PatternEvent& b) { return a.timeSec < b.timeSec; });

    Rng rng (seed);
    Pattern p;
    p.sampleRate = sampleRate;

    std::vector<std::vector<float>> sounds;
    int64_t end = 0;
    for (const auto& e : events)
    {
        const auto v = options.vary ? Variation::random (rng) : Variation {};
        const float amp = (e.amplitude < 0.0f ? defaultAmplitude (e.sound) : e.amplitude)
                          * (float) dbToGain (options.gainDb);

        TruthHit t;
        t.timeSec = e.timeSec;
        t.sample = (int64_t) std::llround (e.timeSec * sampleRate);
        t.sound = e.sound;
        t.peak = amp * (float) dbToGain (v.gainDb);

        sounds.push_back (synthesize (e.sound, sampleRate, amp, rng, v));
        end = std::max (end, t.sample + (int64_t) sounds.back().size());
        p.truth.push_back (t);
    }

    const int64_t total = end + (int64_t) std::llround (options.tailSec * sampleRate);
    if (options.noiseDb > -200.0f)
        p.audio = whiteNoise (total, options.noiseDb, seed ^ 0x5DEECE66Dull);
    else
        p.audio.assign ((size_t) total, 0.0f);

    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const auto start = (size_t) p.truth[i].sample;
        for (size_t j = 0; j < sounds[i].size(); ++j)
            p.audio[start + j] += sounds[i][j];
    }

    return p;
}

Pattern makeMixedPattern (double sampleRate, uint64_t seed, int kicks, int snares, int hats,
                          int clicks, double minGapSec, double maxGapSec, const PatternOptions& options)
{
    std::vector<Sound> order;
    order.insert (order.end(), (size_t) std::max (0, kicks), Sound::kick);
    order.insert (order.end(), (size_t) std::max (0, snares), Sound::snare);
    order.insert (order.end(), (size_t) std::max (0, hats), Sound::hat);
    order.insert (order.end(), (size_t) std::max (0, clicks), Sound::click);

    Rng rng (seed * 7919u + 17u);
    for (int i = (int) order.size() - 1; i > 0; --i)
        std::swap (order[(size_t) i], order[(size_t) rng.uniformInt (0, i)]);

    std::vector<PatternEvent> events;
    double t = 0.2;
    for (auto s : order)
    {
        PatternEvent e;
        e.timeSec = t;
        e.sound = s;
        events.push_back (e);
        t += rng.uniform (minGapSec, maxGapSec);
    }

    return buildPattern (events, sampleRate, seed, options);
}

} // namespace bbrtest
