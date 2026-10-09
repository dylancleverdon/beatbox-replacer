#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "ProfileIO.h"
#include "Updater.h"

#include "../core/Analysis.h"
#include "../core/MidiFile.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>

namespace
{
constexpr int kSidechainBus = BBR_AUX_ONLY_SIDECHAIN ? 0 : 1;
constexpr int kMainInputBus = BBR_AUX_ONLY_SIDECHAIN ? -1 : 0;

constexpr int kMaxHitsPerChunk = 64;
constexpr int kMaxHeldNotes = 128;
constexpr int kMidiChannel = 1;
constexpr int kMinChunk = 256;
constexpr int kMaxChunk = 16384;
constexpr double kDefaultSampleRate = 48000.0;
constexpr double kAuditionBufferSeconds = 4.0;
constexpr double kAuditionHitSeconds = 0.25;
constexpr double kAuditionPreRollSeconds = 0.005;
constexpr double kMaxFileSeconds = 600.0;
constexpr double kMeterReleaseSeconds = 0.3;
constexpr double kJumpToleranceBeats = 0.1;
constexpr float kSignalThreshold = 0.001f; // -60 dBFS
constexpr float kWindowEpsilon = 1.0e-4f;
constexpr juce::uint32 kModelRetireMs = 2000;
constexpr juce::uint32 kWindowDebounceMs = 300;
constexpr juce::uint32 kSignalHoldMs = 2000;
constexpr juce::uint32 kMeterStaleMs = 500;
constexpr int kMaxSlotNameLength = 64;
constexpr int kStateVersion = 1;
constexpr const char* kStateTag = "BeatboxReplacerState";
constexpr const char* kUiTag = "UI";

enum LearnPhase : int
{
    learnIdle,
    learnArmed,
    learnRecording,
    learnStopRequested
};

struct DrumSound
{
    const char* name;
    int note;
};

// The first three are the default slots; addSlot() picks the first unused entry.
constexpr DrumSound kDrumSounds[] = {
    { "Kick", 36 }, { "Snare", 38 }, { "Hi-hat", 42 }, { "Open hat", 46 }, { "Clap", 39 },
    { "Rim", 37 }, { "Tom low", 45 }, { "Tom high", 48 }, { "Crash", 49 }
};

struct Transport
{
    bool hasPosition = false;
    bool playing = false;
    bool ppqFromHost = false;
    bool hasBpm = false, hasTimeSig = false;
    double ppq = 0.0, bpm = 120.0, barStart = 0.0;
    int num = 4, den = 4;

    bool hasTransportInfo() const noexcept { return hasPosition && (ppqFromHost || playing); }
};

int clampOffset (int64_t offset, int numSamples) noexcept
{
    return (int) juce::jlimit<int64_t> (0, (int64_t) juce::jmax (0, numSamples - 1), offset);
}

bool sameWindow (float a, float b) noexcept
{
    return std::abs (a - b) <= kWindowEpsilon;
}

// Keeps the training set within the profile limit by dropping the oldest hits of whichever
// label has the most examples, so no sound loses all of its training.
void limitTrainingSize (std::vector<bbr::TrainingHit>& hits)
{
    while ((int) hits.size() > ProfileIO::kMaxTrainingHits)
    {
        std::map<int, int> counts;

        for (const auto& h : hits)
            ++counts[h.slotId];

        const auto biggest = std::max_element (counts.begin(), counts.end(),
                                               [] (const auto& a, const auto& b) { return a.second < b.second; })->first;
        hits.erase (std::find_if (hits.begin(), hits.end(), [biggest] (const auto& h) { return h.slotId == biggest; }));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::live, 1 }, "Live MIDI", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::threshold, 1 }, "Threshold",
                                                       NormalisableRange<float> (-70.0f, -10.0f, 0.5f), -48.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::rise, 1 }, "Sensitivity",
                                                       NormalisableRange<float> (3.0f, 24.0f, 0.5f), 9.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::minGap, 1 }, "Min gap",
                                                       NormalisableRange<float> (20.0f, 300.0f, 1.0f), 60.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::window, 1 }, "Analysis window",
                                                       NormalisableRange<float> (bbr::kMinWindowMs, bbr::kMaxWindowMs, 1.0f), 20.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms").withAutomatable (false)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::dynamic, 1 }, "Dynamic velocity", true));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::fixedVelocity, 1 }, "Fixed velocity", 1, 127, 100));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::noteLength, 1 }, "Note length",
                                                       NormalisableRange<float> (10.0f, 500.0f, 1.0f), 60.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    return layout;
}

#if BBR_AUX_ONLY_SIDECHAIN
struct AuxOnlyVST3Extensions final : public juce::VST3ClientExtensions
{
    bool getPluginHasMainInput() const override { return false; }
};
#endif
} // namespace

//==================================================================================================
struct BeatboxProcessor::Impl
{
    explicit Impl (juce::AudioProcessorValueTreeState& state)
        : liveParam (state.getRawParameterValue (ParamIDs::live)),
          thresholdParam (state.getRawParameterValue (ParamIDs::threshold)),
          riseParam (state.getRawParameterValue (ParamIDs::rise)),
          minGapParam (state.getRawParameterValue (ParamIDs::minGap)),
          windowParam (state.getRawParameterValue (ParamIDs::window)),
          dynamicParam (state.getRawParameterValue (ParamIDs::dynamic)),
          fixedVelocityParam (state.getRawParameterValue (ParamIDs::fixedVelocity)),
          noteLengthParam (state.getRawParameterValue (ParamIDs::noteLength))
    {
        for (auto& n : slotNotes)
            n.store (-1);

        for (auto& c : liveHitCounts)
            c.store (0);

        for (auto& t : takes)
            t.hits.resize ((size_t) kMaxCaptureHits);

        formats.registerBasicFormats();
    }

    // ---- Parameters (any thread) ----------------------------------------------------------------
    std::atomic<float>* const liveParam;
    std::atomic<float>* const thresholdParam;
    std::atomic<float>* const riseParam;
    std::atomic<float>* const minGapParam;
    std::atomic<float>* const windowParam;
    std::atomic<float>* const dynamicParam;
    std::atomic<float>* const fixedVelocityParam;
    std::atomic<float>* const noteLengthParam;

    static float readParam (const std::atomic<float>* p, float lo, float hi, float fallback) noexcept
    {
        if (p == nullptr)
            return fallback;

        const float v = p->load (std::memory_order_relaxed);
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback;
    }

    bbr::DetectorSettings detectorSettings() const noexcept
    {
        bbr::DetectorSettings s;
        s.thresholdDb = readParam (thresholdParam, -70.0f, -10.0f, -48.0f);
        s.riseDb = readParam (riseParam, 3.0f, 24.0f, 9.0f);
        s.minGapMs = readParam (minGapParam, 20.0f, 300.0f, 60.0f);
        return s;
    }

    bbr::FeatureSettings featureSettings() const noexcept
    {
        bbr::FeatureSettings s;
        s.windowMs = readParam (windowParam, bbr::kMinWindowMs, bbr::kMaxWindowMs, 20.0f);
        return s;
    }

    bool liveEnabled() const noexcept       { return readParam (liveParam, 0.0f, 1.0f, 1.0f) >= 0.5f; }
    bool dynamicVelocity() const noexcept   { return readParam (dynamicParam, 0.0f, 1.0f, 1.0f) >= 0.5f; }
    int fixedVelocity() const noexcept      { return juce::roundToInt (readParam (fixedVelocityParam, 1.0f, 127.0f, 100.0f)); }
    double noteLengthMs() const noexcept    { return (double) readParam (noteLengthParam, 10.0f, 500.0f, 60.0f); }

    // ---- Message thread state -------------------------------------------------------------------
    std::vector<Slot> slots;            // sorted by id
    juce::CriticalSection profileLock;  // writers of slots/training hold it; getStateInformation reads under it
    float trainingWindowMs = 20.0f;     // window the training features were computed with
    bool learnReady = false;
    float lastTakeWindowMs = 20.0f;     // window the last take's features were computed with
    uint32_t lastCollectedTakeGen = 0;
    juce::AudioFormatManager formats;

    std::shared_ptr<const bbr::ClassifierModel> messageModel;

    struct RetiredModel
    {
        std::shared_ptr<const bbr::ClassifierModel> model;
        juce::uint32 since = 0;
    };

    std::vector<RetiredModel> retiredModels;

    LearnState notifiedLearnState = LearnState::idle;
    bool notifiedCaptureRecording = false;
    bool notifiedHostPlaying = false;
    std::atomic<juce::uint32> lastWindowChangeMs { 0 };

    // ---- Model handed to the audio thread --------------------------------------------------------
    juce::SpinLock modelLock;
    std::shared_ptr<const bbr::ClassifierModel> sharedModel; // guarded by modelLock
    std::shared_ptr<const bbr::ClassifierModel> audioModel;  // audio thread only

    std::array<std::atomic<int>, bbr::kMaxSlots> slotNotes;  // -1 = unused

    // ---- Audio thread ----------------------------------------------------------------------------
    std::atomic<double> currentRate { 0.0 };
    bbr::LiveEngine engine;
    std::vector<float> scratch;
    std::array<bbr::LiveHit, kMaxHitsPerChunk> liveHits {};

    struct HeldNote
    {
        int note = 0;
        int64_t offTime = 0;
    };

    std::array<HeldNote, kMaxHeldNotes> held {};
    int numHeld = 0;
    std::atomic<bool> flushNotes { false };
    int64_t audioClock = 0;

    bool prevPlaying = false, prevPpqFromHost = false;
    double prevPpq = 0.0, prevBpm = 120.0, fallbackPpq = 0.0;
    int prevNumSamples = 0;

    // ---- Host info published by the audio thread --------------------------------------------------
    std::atomic<bool> hostPlaying { false };
    std::atomic<bool> hostHasTransport { false };
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<int> hostTimeSigNum { 4 }, hostTimeSigDen { 4 };

    // ---- Learn recording ---------------------------------------------------------------------------
    juce::SpinLock learnLock;
    std::vector<float> learnBuffer;     // guarded by learnLock
    std::atomic<int64_t> learnWritePos { 0 };
    std::atomic<double> learnSampleRate { kDefaultSampleRate };
    std::atomic<int> learnPhase { learnIdle };
    std::atomic<bool> learnFollowsTransport { false };

    // ---- Capture -----------------------------------------------------------------------------------
    // Takes alternate between two buffers; a take's generation number picks its buffer. The timer
    // copies a finished take and then checks that no newer take has reused that buffer meanwhile.
    struct TakeBuffer
    {
        double startPpq = 0.0, barStartPpq = 0.0, bpm = 120.0;
        int timeSigNum = 4, timeSigDen = 4;
        float windowMs = 20.0f;
        std::vector<bbr::CapturedHit> hits;  // kMaxCaptureHits, allocated in the constructor
        std::atomic<int> count { 0 };
    };

    std::atomic<bool> captureArmed { false };
    std::array<TakeBuffer, 2> takes;
    std::atomic<uint32_t> takeStartedGen { 0 }, takeFinishedGen { 0 };
    std::atomic<bool> captureRecording { false };
    bool captureActive = false;         // audio thread
    uint32_t captureGen = 0;            // audio thread: generation of the active take

    // ---- Audition ----------------------------------------------------------------------------------
    juce::SpinLock auditionLock;
    std::vector<float> auditionBuffer;  // guarded by auditionLock
    int auditionLength = 0, auditionPos = 0;

    // ---- Meters ------------------------------------------------------------------------------------
    float meterLevel = 0.0f;            // audio thread
    std::atomic<float> inputLevel { 0.0f };
    std::atomic<juce::uint32> lastBlockMs { 0 }, lastSignalMs { 0 };
    std::atomic<bool> hasHadSignal { false };
    std::array<std::atomic<uint32_t>, bbr::kMaxSlots + 2> liveHitCounts; // [kMaxSlots] ignore, [+1] unassigned
    std::atomic<int> lastLiveHitSlot { bbr::kUnassigned };

    //==============================================================================================
    // Audio thread helpers

    Transport readTransport (juce::AudioPlayHead* head, double rate) const noexcept
    {
        Transport t;

        if (head == nullptr)
            return t;

        const auto pos = head->getPosition();

        if (! pos.hasValue())
            return t;

        t.hasPosition = true;
        t.playing = pos->getIsPlaying();
        t.bpm = hostBpm.load (std::memory_order_relaxed);
        t.num = hostTimeSigNum.load (std::memory_order_relaxed);
        t.den = hostTimeSigDen.load (std::memory_order_relaxed);

        if (const auto tempo = pos->getBpm(); tempo.hasValue() && std::isfinite (*tempo) && *tempo >= 1.0 && *tempo <= 999.0)
        {
            t.bpm = *tempo;
            t.hasBpm = true;
        }

        if (const auto sig = pos->getTimeSignature();
            sig.hasValue() && sig->numerator >= 1 && sig->numerator <= 64 && sig->denominator >= 1 && sig->denominator <= 64)
        {
            t.num = sig->numerator;
            t.den = sig->denominator;
            t.hasTimeSig = true;
        }

        const auto hostPpq = pos->getPpqPosition();

        if (hostPpq.hasValue() && std::isfinite (*hostPpq))
        {
            t.ppq = *hostPpq;
            t.ppqFromHost = true;
        }
        else if (const auto seconds = pos->getTimeInSeconds(); seconds.hasValue() && std::isfinite (*seconds))
        {
            t.ppq = *seconds * t.bpm / 60.0;
            t.ppqFromHost = true;
        }
        else if (const auto samples = pos->getTimeInSamples(); samples.hasValue())
        {
            t.ppq = (double) *samples / rate * t.bpm / 60.0;
            t.ppqFromHost = true;
        }
        else
        {
            t.ppq = t.playing ? fallbackPpq : 0.0;
        }

        const double barLength = bbr::quarterNotesPerBar (t.num, t.den);
        t.barStart = std::floor (t.ppq / barLength + 1.0e-9) * barLength;

        if (const auto lastBar = pos->getPpqPositionOfLastBarStart();
            lastBar.hasValue() && hostPpq.hasValue() && std::isfinite (*lastBar)
            && *lastBar <= t.ppq + 1.0e-6 && t.ppq - *lastBar < barLength + 1.0e-6)
        {
            t.barStart = *lastBar;
        }

        return t;
    }

    void publishTransport (const Transport& t) noexcept
    {
        hostPlaying.store (t.playing, std::memory_order_relaxed);
        hostHasTransport.store (t.hasTransportInfo(), std::memory_order_relaxed);

        if (t.hasBpm)
            hostBpm.store (t.bpm, std::memory_order_relaxed);

        if (t.hasTimeSig)
        {
            hostTimeSigNum.store (t.num, std::memory_order_relaxed);
            hostTimeSigDen.store (t.den, std::memory_order_relaxed);
        }
    }

    // True if the host position moved somewhere other than where this block should start
    // (loop wrap, locate while playing).
    bool transportJumped (const Transport& t, double rate) const noexcept
    {
        if (! prevPlaying || ! t.playing || ! t.ppqFromHost || ! prevPpqFromHost)
            return false;

        const double expected = prevPpq + (double) prevNumSamples / rate * prevBpm / 60.0;
        return std::abs (t.ppq - expected) > kJumpToleranceBeats;
    }

    // Learn transport edges. Returns true if this block's audio belongs in the learn recording.
    bool updateLearn (const Transport& t, double rate, bool analysing) noexcept
    {
        int phase = learnPhase.load();

        if (phase == learnArmed && t.playing && analysing)
        {
            learnSampleRate.store (rate);

            if (learnPhase.compare_exchange_strong (phase, learnRecording))
                phase = learnRecording;
        }
        else if (phase == learnRecording && ! t.playing && learnFollowsTransport.load())
        {
            if (learnPhase.compare_exchange_strong (phase, learnStopRequested))
                phase = learnStopRequested;
        }

        return analysing && phase == learnRecording;
    }

    void writeLearn (const float* x, int n) noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (learnLock);

        if (! lock.isLocked())
            return;

        const auto capacity = (int64_t) learnBuffer.size();
        const auto pos = learnWritePos.load (std::memory_order_relaxed);
        const auto count = juce::jlimit<int64_t> (0, juce::jmax<int64_t> (0, capacity - pos), (int64_t) n);

        if (count > 0)
        {
            std::copy (x, x + count, learnBuffer.data() + pos);
            learnWritePos.store (pos + count);
        }

        if (pos + count >= capacity)
        {
            int expected = learnRecording;
            learnPhase.compare_exchange_strong (expected, learnStopRequested);
        }
    }

    void startTake (const Transport& t, float windowMs) noexcept
    {
        const uint32_t gen = takeStartedGen.load (std::memory_order_relaxed) + 1u;
        takeStartedGen.store (gen, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);

        auto& tb = takes[gen & 1u];
        tb.count.store (0, std::memory_order_relaxed);
        tb.startPpq = t.ppq;
        tb.barStartPpq = juce::jmax (0.0, t.barStart); // a count-in before the song start still drops at bar 1
        tb.bpm = t.bpm;
        tb.timeSigNum = t.num;
        tb.timeSigDen = t.den;
        tb.windowMs = windowMs;

        captureGen = gen;
        captureActive = true;
        captureRecording.store (true);
    }

    void finishTake() noexcept
    {
        takeFinishedGen.store (captureGen, std::memory_order_release);
        captureActive = false;
        captureRecording.store (false);
    }

    void updateCapture (const Transport& t, double rate, bool analysing, float windowMs) noexcept
    {
        const bool armed = captureArmed.load();

        if (captureActive && (! t.playing || ! armed || transportJumped (t, rate)))
            finishTake();

        if (! captureActive && armed && t.playing && analysing)
            startTake (t, windowMs);
    }

    void addCapturedHit (const bbr::LiveHit& hit, double ppq) noexcept
    {
        auto& tb = takes[captureGen & 1u];
        const int count = tb.count.load (std::memory_order_relaxed);

        if (count >= kMaxCaptureHits)
            return;

        auto& dest = tb.hits[(size_t) count];
        dest.ppq = ppq;
        dest.slotId = hit.result.slotId;
        dest.peakDb = hit.peakDb;
        dest.features = hit.features;
        tb.count.store (count + 1, std::memory_order_release);
    }

    void releaseNotesDueBy (juce::MidiBuffer& midi, int64_t time, int64_t blockStart, int numSamples) noexcept
    {
        int kept = 0;

        for (int i = 0; i < numHeld; ++i)
        {
            const auto h = held[(size_t) i];

            if (h.offTime <= time)
                midi.addEvent (juce::MidiMessage::noteOff (kMidiChannel, h.note), clampOffset (h.offTime - blockStart, numSamples));
            else
                held[(size_t) kept++] = h;
        }

        numHeld = kept;
    }

    void releaseNote (juce::MidiBuffer& midi, int note, int pos) noexcept
    {
        for (int i = 0; i < numHeld; ++i)
        {
            if (held[(size_t) i].note == note)
            {
                midi.addEvent (juce::MidiMessage::noteOff (kMidiChannel, note), pos);
                std::copy (held.begin() + i + 1, held.begin() + numHeld, held.begin() + i);
                --numHeld;
                return;
            }
        }
    }

    void releaseAll (juce::MidiBuffer& midi, int pos) noexcept
    {
        for (int i = 0; i < numHeld; ++i)
            midi.addEvent (juce::MidiMessage::noteOff (kMidiChannel, held[(size_t) i].note), pos);

        numHeld = 0;
    }

    void holdNote (juce::MidiBuffer& midi, int note, int64_t offTime, int pos) noexcept
    {
        if (numHeld == kMaxHeldNotes)
            releaseNote (midi, held[0].note, pos);

        held[(size_t) numHeld++] = HeldNote { note, offTime };
    }

    struct Block
    {
        int numSamples = 0;
        double rate = kDefaultSampleRate;
        int64_t clockStart = 0;          // audioClock at the block start
        int64_t engineStart = 0;         // engine clock at the block start
        double ppq = 0.0, bpm = 120.0;
        const bbr::ClassifierModel* model = nullptr;
        bool liveOn = true, dynamic = true;
        int fixedVelocity = 100;
        int64_t noteLength = 1;          // samples
    };

    void handleHit (const bbr::LiveHit& hit, int chunkStart, const Block& b, juce::MidiBuffer& midi) noexcept
    {
        const int slot = hit.result.slotId;
        const bool isSlot = slot >= 0 && slot < bbr::kMaxSlots;
        const int counter = isSlot ? slot : (slot == bbr::kIgnoreSlotId ? bbr::kMaxSlots : bbr::kMaxSlots + 1);

        liveHitCounts[(size_t) counter].fetch_add (1u, std::memory_order_relaxed);
        lastLiveHitSlot.store (slot, std::memory_order_relaxed);

        if (isSlot && b.liveOn)
        {
            const int note = slotNotes[(size_t) slot].load (std::memory_order_relaxed);

            if (note >= 0 && note <= 127)
            {
                const int pos = clampOffset ((int64_t) chunkStart + hit.emitOffset, b.numSamples);
                const int64_t when = b.clockStart + pos;

                // Offs due before (or at) this note go first; a still-held instance of the same
                // note is cut here so the new note-on is never swallowed.
                releaseNotesDueBy (midi, when, b.clockStart, b.numSamples);
                releaseNote (midi, note, pos);

                const float reference = b.model != nullptr ? b.model->getMedianPeakDb (slot)
                                                           : std::numeric_limits<float>::quiet_NaN();
                const int velocity = bbr::velocityFor (hit.peakDb, reference, b.dynamic, b.fixedVelocity);
                midi.addEvent (juce::MidiMessage::noteOn (kMidiChannel, note, (juce::uint8) velocity), pos);
                holdNote (midi, note, when + b.noteLength, pos);
            }
        }

        if (captureActive)
            addCapturedHit (hit, b.ppq + (double) (hit.onsetSample - b.engineStart) / b.rate * b.bpm / 60.0);
    }

    void addAudition (juce::AudioBuffer<float>& buffer, int firstChannel, int numChannels, int numSamples) noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (auditionLock);

        if (! lock.isLocked() || auditionPos >= auditionLength)
            return;

        const int n = juce::jmin (numSamples, auditionLength - auditionPos);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.addFrom (firstChannel + ch, 0, auditionBuffer.data() + auditionPos, n);

        auditionPos += n;
    }

    void updateMeter (float blockPeak, int numSamples, double rate) noexcept
    {
        const auto decay = (float) std::exp (-(double) numSamples / (kMeterReleaseSeconds * rate));
        meterLevel = juce::jmax (blockPeak, meterLevel * decay);
        inputLevel.store (meterLevel, std::memory_order_relaxed);

        const auto now = juce::Time::getMillisecondCounter();
        lastBlockMs.store (now, std::memory_order_relaxed);

        if (blockPeak > kSignalThreshold)
        {
            lastSignalMs.store (now, std::memory_order_relaxed);
            hasHadSignal.store (true, std::memory_order_relaxed);
        }
    }

    void endBlock (const Transport& t, int numSamples, double rate) noexcept
    {
        prevPlaying = t.playing;
        prevPpqFromHost = t.ppqFromHost;
        prevPpq = t.ppq;
        prevBpm = t.bpm;
        prevNumSamples = numSamples;
        fallbackPpq = t.playing ? t.ppq + (double) numSamples / rate * t.bpm / 60.0 : 0.0;
        audioClock += numSamples;
    }

    // Channels of input bus `bus` inside the process buffer (0 if it doesn't exist / is disabled).
    static int inputBusChannels (const juce::AudioProcessor& p, const juce::AudioBuffer<float>& buffer,
                                 int bus, int& firstChannel) noexcept
    {
        firstChannel = 0;

        if (bus < 0 || bus >= p.getBusCount (true))
            return 0;

        firstChannel = p.getChannelIndexInProcessBlockBuffer (true, bus, 0);
        return juce::jlimit (0, juce::jmax (0, buffer.getNumChannels() - firstChannel), p.getChannelCountOfBus (true, bus));
    }
};

//==================================================================================================
BeatboxProcessor::BeatboxProcessor()
    : AudioProcessor (BusesProperties()
                     #if ! BBR_AUX_ONLY_SIDECHAIN
                      .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                     #endif
                      .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Params", createParameterLayout()),
      impl (std::make_unique<Impl> (apvts))
{
    static std::atomic<bool> cleanedUp { false };

    if (! cleanedUp.exchange (true))
        Updater::cleanupAfterPreviousUpdate();

    auto& d = *impl;

    for (int id = 0; id < 3; ++id)
    {
        Slot slot;
        slot.id = id;
        slot.name = kDrumSounds[id].name;
        slot.note = kDrumSounds[id].note;
        slot.colour = getSlotColour (id);
        d.slots.push_back (slot);
        d.slotNotes[(size_t) id].store (slot.note);
    }

    d.trainingWindowMs = currentFeatureSettings().windowMs;
    apvts.addParameterListener (ParamIDs::window, this);

    const auto defaultProfile = getDefaultProfileFile();

    if (defaultProfile.existsAsFile())
        importProfile (defaultProfile); // a broken default profile just leaves the defaults

    if (d.messageModel == nullptr)
        rebuildModel();

    startTimerHz (30);
}

BeatboxProcessor::~BeatboxProcessor()
{
    stopTimer();
    apvts.removeParameterListener (ParamIDs::window, this);
}

#if BBR_AUX_ONLY_SIDECHAIN
juce::VST3ClientExtensions* BeatboxProcessor::getVST3ClientExtensions()
{
    static AuxOnlyVST3Extensions extensions;
    return &extensions;
}
#endif

//==================================================================================================
void BeatboxProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    auto& d = *impl;
    const double rate = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
    const int chunk = juce::jlimit (kMinChunk, kMaxChunk, samplesPerBlock);

    d.scratch.assign ((size_t) chunk, 0.0f);
    d.engine.prepare (rate, chunk);
    d.engine.setDetectorSettings (d.detectorSettings());
    d.engine.setFallbackFeatureSettings (d.featureSettings());
    d.currentRate.store (rate);

    // A learn recording can't mix two sample rates: finish it at the old one.
    if (d.learnPhase.load() == learnRecording)
    {
        if (d.learnWritePos.load() == 0)
        {
            d.learnSampleRate.store (rate);
        }
        else if (std::abs (d.learnSampleRate.load() - rate) > 0.5)
        {
            int expected = learnRecording;
            d.learnPhase.compare_exchange_strong (expected, learnStopRequested);
        }
    }

    {
        const juce::SpinLock::ScopedLockType lock (d.auditionLock);
        d.auditionBuffer.assign ((size_t) std::ceil (kAuditionBufferSeconds * rate), 0.0f);
        d.auditionLength = 0;
        d.auditionPos = 0;
    }

    d.meterLevel = 0.0f;
    d.flushNotes.store (true);
}

void BeatboxProcessor::releaseResources()
{
    // Held notes get their note-offs at the start of the next block.
    impl->flushNotes.store (true);
}

bool BeatboxProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto isMonoOrStereo = [] (const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };

    if (layouts.outputBuses.size() != 1 || ! isMonoOrStereo (layouts.outputBuses.getReference (0)))
        return false;

   #if BBR_AUX_ONLY_SIDECHAIN
    return layouts.inputBuses.size() == 1 && isMonoOrStereo (layouts.inputBuses.getReference (0));
   #else
    if (layouts.inputBuses.size() != 2)
        return false;

    const auto& mainIn = layouts.inputBuses.getReference (0);
    return (mainIn.isDisabled() || isMonoOrStereo (mainIn)) && isMonoOrStereo (layouts.inputBuses.getReference (1));
   #endif
}

void BeatboxProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear(); // the VST3 wrapper uses the same buffer for MIDI in and out

    auto& d = *impl;
    const int numSamples = buffer.getNumSamples();
    const double rate = d.currentRate.load (std::memory_order_relaxed);

    if (numSamples <= 0 || d.scratch.empty() || ! (rate > 0.0))
    {
        buffer.clear();
        return;
    }

    if (d.flushNotes.exchange (false))
        d.releaseAll (midi, 0);

    const auto transport = d.readTransport (getPlayHead(), rate);
    d.publishTransport (transport);

    {
        // If the message thread is swapping models right now, keep last block's one.
        const juce::SpinLock::ScopedTryLockType lock (d.modelLock);

        if (lock.isLocked() && d.audioModel != d.sharedModel)
            d.audioModel = d.sharedModel;
    }

    const bbr::ClassifierModel* model = (d.audioModel != nullptr && ! d.audioModel->empty()) ? d.audioModel.get() : nullptr;
    const auto fallbackFeatures = d.featureSettings();
    d.engine.setDetectorSettings (d.detectorSettings());
    d.engine.setFallbackFeatureSettings (fallbackFeatures);

    const float windowMs = model != nullptr ? model->getFeatureSettings().windowMs : fallbackFeatures.windowMs;
    const bool recordLearn = d.updateLearn (transport, rate, true);
    d.updateCapture (transport, rate, true, windowMs);

    Impl::Block b;
    b.numSamples = numSamples;
    b.rate = rate;
    b.clockStart = d.audioClock;
    b.engineStart = d.engine.getSamplesProcessed();
    b.ppq = transport.ppq;
    b.bpm = transport.bpm;
    b.model = model;
    b.liveOn = d.liveEnabled();
    b.dynamic = d.dynamicVelocity();
    b.fixedVelocity = d.fixedVelocity();
    b.noteLength = juce::jmax<int64_t> (1, (int64_t) std::llround (d.noteLengthMs() * 0.001 * rate));

    // Analysis input: the sidechain, else the main input, else silence. Read everything before
    // the output is touched (the buffers are shared in place).
    int firstChannel = 0;
    int numChannels = Impl::inputBusChannels (*this, buffer, kSidechainBus, firstChannel);

    if (numChannels == 0)
        numChannels = Impl::inputBusChannels (*this, buffer, kMainInputBus, firstChannel);

    const int chunkSize = (int) d.scratch.size();
    float blockPeak = 0.0f;

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - start);
        float* const mono = d.scratch.data();

        if (numChannels > 0)
        {
            juce::FloatVectorOperations::copy (mono, buffer.getReadPointer (firstChannel, start), n);

            for (int ch = 1; ch < numChannels; ++ch)
                juce::FloatVectorOperations::add (mono, buffer.getReadPointer (firstChannel + ch, start), n);

            if (numChannels > 1)
                juce::FloatVectorOperations::multiply (mono, 1.0f / (float) numChannels, n);

            for (int i = 0; i < n; ++i)
                if (! std::isfinite (mono[i]))
                    mono[i] = 0.0f; // a NaN would poison the detector's filter state for good
        }
        else
        {
            juce::FloatVectorOperations::clear (mono, n);
        }

        const auto range = juce::FloatVectorOperations::findMinAndMax (mono, n);
        blockPeak = juce::jmax (blockPeak, -range.getStart(), range.getEnd());

        if (recordLearn)
            d.writeLearn (mono, n);

        const int found = d.engine.process (mono, n, model, d.liveHits.data(), kMaxHitsPerChunk);

        for (int i = 0; i < found; ++i)
            d.handleHit (d.liveHits[(size_t) i], start, b, midi);
    }

    d.releaseNotesDueBy (midi, d.audioClock + numSamples - 1, d.audioClock, numSamples);

    buffer.clear();

    if (getBusCount (false) > 0)
    {
        const int outFirst = getChannelIndexInProcessBlockBuffer (false, 0, 0);
        const int outChannels = juce::jlimit (0, juce::jmax (0, buffer.getNumChannels() - outFirst), getChannelCountOfBus (false, 0));
        d.addAudition (buffer, outFirst, outChannels, numSamples);
    }

    d.updateMeter (blockPeak, numSamples, rate);
    d.endBlock (transport, numSamples, rate);
}

void BeatboxProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    buffer.clear();

    auto& d = *impl;
    d.flushNotes.store (false);
    d.releaseAll (midi, 0);

    const double rate = d.currentRate.load (std::memory_order_relaxed);

    if (! (rate > 0.0))
        return;

    // Keep following the transport so recordings still end when it stops.
    const auto transport = d.readTransport (getPlayHead(), rate);
    d.publishTransport (transport);
    d.updateLearn (transport, rate, false);
    d.updateCapture (transport, rate, false, d.featureSettings().windowMs);
    d.endBlock (transport, buffer.getNumSamples(), rate);
}

juce::AudioProcessorEditor* BeatboxProcessor::createEditor()
{
    return new BeatboxEditor (*this);
}

//==================================================================================================
bbr::DetectorSettings BeatboxProcessor::currentDetectorSettings() const
{
    return impl->detectorSettings();
}

bbr::FeatureSettings BeatboxProcessor::currentFeatureSettings() const
{
    return impl->featureSettings();
}

void BeatboxProcessor::parameterChanged (const juce::String& parameterID, float)
{
    // Any thread. The timer rebuilds the model once the value has settled.
    if (parameterID == ParamIDs::window)
        impl->lastWindowChangeMs.store (juce::Time::getMillisecondCounter());
}

void BeatboxProcessor::timerCallback()
{
    auto& d = *impl;
    const auto now = juce::Time::getMillisecondCounter();
    bool changed = false;

    // Replaced models are released only when the audio thread no longer holds them, so it never
    // drops the last reference.
    d.retiredModels.erase (std::remove_if (d.retiredModels.begin(), d.retiredModels.end(),
                                           [now] (const Impl::RetiredModel& r)
                                           {
                                               return now - r.since >= kModelRetireMs && r.model.use_count() == 1;
                                           }),
                           d.retiredModels.end());

    if (d.learnPhase.load() == learnStopRequested)
    {
        finishLearnRecording();
        changed = true;
    }

    if (collectFinishedTake())
        changed = true;

    if (d.messageModel != nullptr
        && ! sameWindow (d.messageModel->getFeatureSettings().windowMs, currentFeatureSettings().windowMs)
        && now - d.lastWindowChangeMs.load() >= kWindowDebounceMs)
    {
        rebuildModel();
        changed = true;
    }

    const auto learnState = getLearnState();
    const bool captureRecording = isCaptureRecording();
    const bool playing = isHostPlaying();

    if (learnState != d.notifiedLearnState || captureRecording != d.notifiedCaptureRecording || playing != d.notifiedHostPlaying)
    {
        d.notifiedLearnState = learnState;
        d.notifiedCaptureRecording = captureRecording;
        d.notifiedHostPlaying = playing;
        changed = true;
    }

    if (changed)
        sendChangeMessage();
}

void BeatboxProcessor::rebuildModel()
{
    auto& d = *impl;
    const auto features = currentFeatureSettings();

    if (! sameWindow (d.trainingWindowMs, features.windowMs))
    {
        auto updated = training;
        bbr::recomputeFeatures (updated, features);

        {
            const juce::ScopedLock lock (d.profileLock);
            training.swap (updated);
        }

        d.trainingWindowMs = features.windowMs;
    }

    std::shared_ptr<const bbr::ClassifierModel> model = bbr::ClassifierModel::build (training, features);
    std::shared_ptr<const bbr::ClassifierModel> previous;

    {
        const juce::SpinLock::ScopedLockType lock (d.modelLock);
        previous = d.sharedModel;
        d.sharedModel = model;
    }

    d.messageModel = model;

    if (previous != nullptr)
        d.retiredModels.push_back ({ std::move (previous), juce::Time::getMillisecondCounter() });

    // Captured features are only comparable when they were measured with the same window.
    if (! model->empty() && hasTake() && sameWindow (d.lastTakeWindowMs, features.windowMs))
        bbr::reclassifyTake (lastTake, *model);
}

std::shared_ptr<const bbr::ClassifierModel> BeatboxProcessor::getModel() const
{
    return impl->messageModel;
}

//==================================================================================================
std::vector<BeatboxProcessor::Slot> BeatboxProcessor::getSlots() const
{
    return impl->slots;
}

std::vector<bbr::SlotInfo> BeatboxProcessor::getSlotInfos() const
{
    std::vector<bbr::SlotInfo> infos;
    infos.reserve (impl->slots.size());

    for (const auto& s : impl->slots)
    {
        bbr::SlotInfo info;
        info.id = s.id;
        info.name = s.name.toStdString();
        info.note = s.note;
        infos.push_back (std::move (info));
    }

    return infos;
}

bool BeatboxProcessor::hasSlot (int id) const
{
    return std::any_of (impl->slots.begin(), impl->slots.end(), [id] (const Slot& s) { return s.id == id; });
}

juce::String BeatboxProcessor::getSlotName (int id) const
{
    if (id == bbr::kIgnoreSlotId)
        return "Ignore";

    for (const auto& s : impl->slots)
        if (s.id == id)
            return s.name;

    return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")); // em dash
}

juce::Colour BeatboxProcessor::getSlotColour (int id)
{
    static const juce::uint32 palette[bbr::kMaxSlots] = {
        0xffff6b4a, // coral
        0xff4aa8ff, // blue
        0xfff5c542, // yellow
        0xff7ed957, // green
        0xffc77dff, // violet
        0xffff7eb6, // pink
        0xff4ae0d0, // teal
        0xffffa24a  // orange
    };

    if (id >= 0 && id < bbr::kMaxSlots)
        return juce::Colour (palette[id]);

    return id == bbr::kIgnoreSlotId ? juce::Colour (0xff8a8a8a) : juce::Colour (0xff5c5c5c);
}

int BeatboxProcessor::addSlot()
{
    auto& d = *impl;

    if ((int) d.slots.size() >= bbr::kMaxSlots)
        return -1;

    int id = 0;

    while (hasSlot (id))
        ++id;

    std::set<int> usedNotes;

    for (const auto& s : d.slots)
        usedNotes.insert (s.note);

    juce::String name;
    int note = -1;

    for (const auto& sound : kDrumSounds)
    {
        const bool nameUsed = std::any_of (d.slots.begin(), d.slots.end(),
                                           [&sound] (const Slot& s) { return s.name.equalsIgnoreCase (sound.name); });

        if (! nameUsed && usedNotes.count (sound.note) == 0)
        {
            name = sound.name;
            note = sound.note;
            break;
        }
    }

    if (note < 0)
    {
        name = "Sound " + juce::String (id + 1);

        for (int candidate = 36; candidate < 128 && note < 0; ++candidate)
            if (usedNotes.count (candidate) == 0)
                note = candidate;

        if (note < 0)
            note = 36;
    }

    Slot slot;
    slot.id = id;
    slot.name = name;
    slot.note = note;
    slot.colour = getSlotColour (id);

    {
        const juce::ScopedLock lock (d.profileLock);
        const auto pos = std::lower_bound (d.slots.begin(), d.slots.end(), id, [] (const Slot& s, int value) { return s.id < value; });
        d.slots.insert (pos, slot);
    }

    d.slotNotes[(size_t) id].store (note);
    sendChangeMessage();
    return id;
}

void BeatboxProcessor::removeSlot (int id)
{
    auto& d = *impl;

    if (! hasSlot (id))
        return;

    {
        const juce::ScopedLock lock (d.profileLock);
        d.slots.erase (std::remove_if (d.slots.begin(), d.slots.end(), [id] (const Slot& s) { return s.id == id; }), d.slots.end());
        training.erase (std::remove_if (training.begin(), training.end(),
                                        [id] (const bbr::TrainingHit& h) { return h.slotId == id; }),
                        training.end());
    }

    d.slotNotes[(size_t) id].store (-1);
    learnSession.forgetSlot (id);
    rebuildModel();
    sendChangeMessage();
}

void BeatboxProcessor::setSlotName (int id, const juce::String& name)
{
    const auto trimmed = name.trim().substring (0, kMaxSlotNameLength);

    if (trimmed.isEmpty())
        return;

    auto& d = *impl;

    for (auto& s : d.slots)
    {
        if (s.id == id && s.name != trimmed)
        {
            {
                const juce::ScopedLock lock (d.profileLock);
                s.name = trimmed;
            }

            sendChangeMessage();
            return;
        }
    }
}

void BeatboxProcessor::setSlotNote (int id, int note)
{
    auto& d = *impl;
    const int clamped = juce::jlimit (0, 127, note);

    for (auto& s : d.slots)
    {
        if (s.id == id && s.note != clamped)
        {
            {
                const juce::ScopedLock lock (d.profileLock);
                s.note = clamped;
            }

            d.slotNotes[(size_t) id].store (clamped);
            sendChangeMessage();
            return;
        }
    }
}

//==================================================================================================
void BeatboxProcessor::startLearn()
{
    auto& d = *impl;
    cancelLearnRecording();

    const double rateNow = d.currentRate.load();
    const double rate = rateNow > 0.0 ? rateNow : kDefaultSampleRate;
    std::vector<float> fresh ((size_t) std::ceil (kMaxLearnSeconds * rate), 0.0f);

    {
        const juce::SpinLock::ScopedLockType lock (d.learnLock);
        std::swap (d.learnBuffer, fresh);
        d.learnWritePos.store (0);
    }

    d.learnSampleRate.store (rate);

    // With a transport, recording follows play/stop; without one it starts right away.
    const bool followTransport = d.hostHasTransport.load();
    d.learnFollowsTransport.store (followTransport);
    d.learnPhase.store (followTransport ? learnArmed : learnRecording);
    sendChangeMessage();
}

void BeatboxProcessor::stopLearn()
{
    if (impl->learnPhase.load() != learnIdle)
        finishLearnRecording();
}

void BeatboxProcessor::cancelLearnRecording()
{
    auto& d = *impl;
    d.learnPhase.store (learnIdle);
    std::vector<float> released;

    {
        const juce::SpinLock::ScopedLockType lock (d.learnLock);
        std::swap (d.learnBuffer, released);
    }
}

void BeatboxProcessor::finishLearnRecording()
{
    auto& d = *impl;
    const int phase = d.learnPhase.exchange (learnIdle);
    std::vector<float> recorded;
    int64_t length = 0;

    {
        const juce::SpinLock::ScopedLockType lock (d.learnLock);
        std::swap (d.learnBuffer, recorded);
        length = juce::jlimit<int64_t> (0, (int64_t) recorded.size(), d.learnWritePos.load());
    }

    if (phase == learnArmed || phase == learnIdle || length <= 0)
    {
        sendChangeMessage(); // nothing recorded: back to the previous state
        return;
    }

    std::vector<float> audio (recorded.begin(), recorded.begin() + length);
    recorded = std::vector<float>();
    analyseLearnAudio (std::move (audio), d.learnSampleRate.load());
}

void BeatboxProcessor::analyseLearnAudio (std::vector<float> mono, double sampleRate)
{
    auto& d = *impl;
    learnSession.setAudio (std::move (mono), sampleRate);
    learnSession.analyze (currentDetectorSettings(), currentFeatureSettings(), getSlotInfos(), d.messageModel.get());
    d.learnReady = true;
    sendChangeMessage();
}

juce::String BeatboxProcessor::learnFromFile (const juce::File& file)
{
    std::vector<float> mono;
    double rate = 0.0;
    const auto error = readAudioFile (file, mono, rate);

    if (error.isNotEmpty())
        return error;

    cancelLearnRecording();
    impl->learnSampleRate.store (rate);
    impl->learnWritePos.store ((int64_t) mono.size());
    analyseLearnAudio (std::move (mono), rate);
    return {};
}

BeatboxProcessor::LearnState BeatboxProcessor::getLearnState() const
{
    switch (impl->learnPhase.load())
    {
        case learnArmed:
            return LearnState::armed;

        case learnRecording:
        case learnStopRequested:
            return LearnState::recording;

        default:
            break;
    }

    return impl->learnReady ? LearnState::ready : LearnState::idle;
}

double BeatboxProcessor::getLearnRecordedSeconds() const
{
    const double rate = impl->learnSampleRate.load();
    return rate > 0.0 ? (double) impl->learnWritePos.load() / rate : 0.0;
}

void BeatboxProcessor::reclusterLearnSession (int k)
{
    if (! impl->learnReady)
        return;

    learnSession.recluster (k, getSlotInfos(), impl->messageModel.get());
    sendChangeMessage();
}

void BeatboxProcessor::setLearnGroupSlot (int group, int slotId)
{
    if (slotId != bbr::kIgnoreSlotId && slotId != bbr::kUnassigned && ! hasSlot (slotId))
        return;

    learnSession.setGroupSlot (group, slotId);
    sendChangeMessage();
}

void BeatboxProcessor::setLearnHitSlot (int hit, int slotId)
{
    if (slotId != bbr::kIgnoreSlotId && slotId != bbr::kUnassigned && ! hasSlot (slotId))
        return;

    learnSession.setHitOverride (hit, slotId);
    sendChangeMessage();
}

void BeatboxProcessor::commitLearnSession (bool replaceExisting)
{
    auto& d = *impl;

    if (! d.learnReady)
        return;

    auto newHits = learnSession.makeTrainingHits();
    newHits.erase (std::remove_if (newHits.begin(), newHits.end(),
                                   [this] (const bbr::TrainingHit& h) { return h.slotId != bbr::kIgnoreSlotId && ! hasSlot (h.slotId); }),
                   newHits.end());

    const float sessionWindow = learnSession.getFeatureSettings().windowMs;

    if (replaceExisting)
    {
        const juce::ScopedLock lock (d.profileLock);
        training = std::move (newHits);
        limitTrainingSize (training);
        d.trainingWindowMs = sessionWindow;
    }
    else
    {
        // New hits must be measured like the existing ones.
        if (! sameWindow (sessionWindow, d.trainingWindowMs))
        {
            bbr::FeatureSettings settings;
            settings.windowMs = d.trainingWindowMs;
            bbr::recomputeFeatures (newHits, settings);
        }

        const juce::ScopedLock lock (d.profileLock);
        training.insert (training.end(), std::make_move_iterator (newHits.begin()), std::make_move_iterator (newHits.end()));
        limitTrainingSize (training);
    }

    rebuildModel();
    learnSession.clear();
    d.learnReady = false;
    sendChangeMessage();
}

void BeatboxProcessor::discardLearnSession()
{
    cancelLearnRecording();
    learnSession.clear();
    impl->learnReady = false;
    sendChangeMessage();
}

//==================================================================================================
int BeatboxProcessor::getTrainingCount (int slotId) const
{
    return (int) std::count_if (training.begin(), training.end(), [slotId] (const bbr::TrainingHit& h) { return h.slotId == slotId; });
}

void BeatboxProcessor::clearTraining()
{
    {
        const juce::ScopedLock lock (impl->profileLock);
        training.clear();
    }

    rebuildModel();
    sendChangeMessage();
}

void BeatboxProcessor::clearTrainingFor (int slotId)
{
    {
        const juce::ScopedLock lock (impl->profileLock);
        training.erase (std::remove_if (training.begin(), training.end(),
                                        [slotId] (const bbr::TrainingHit& h) { return h.slotId == slotId; }),
                        training.end());
    }

    rebuildModel();
    sendChangeMessage();
}

//==================================================================================================
void BeatboxProcessor::setCaptureArmed (bool shouldBeArmed)
{
    if (impl->captureArmed.exchange (shouldBeArmed) != shouldBeArmed)
        sendChangeMessage();
}

bool BeatboxProcessor::isCaptureArmed() const
{
    return impl->captureArmed.load();
}

bool BeatboxProcessor::isCaptureRecording() const
{
    return impl->captureRecording.load();
}

bool BeatboxProcessor::collectFinishedTake()
{
    auto& d = *impl;
    const uint32_t finished = d.takeFinishedGen.load (std::memory_order_acquire);

    if (finished == d.lastCollectedTakeGen)
        return false;

    d.lastCollectedTakeGen = finished;
    const auto& tb = d.takes[finished & 1u];

    bbr::CaptureTake take;
    take.startPpq = tb.startPpq;
    take.barStartPpq = tb.barStartPpq;
    take.bpm = tb.bpm;
    take.timeSigNum = tb.timeSigNum;
    take.timeSigDen = tb.timeSigDen;
    const float windowMs = tb.windowMs;
    const int count = juce::jlimit (0, kMaxCaptureHits, tb.count.load (std::memory_order_acquire));
    take.hits.assign (tb.hits.begin(), tb.hits.begin() + count);

    // If a newer pass has started writing into the same buffer meanwhile, the copy is unusable.
    std::atomic_thread_fence (std::memory_order_acquire);

    if (d.takeStartedGen.load (std::memory_order_relaxed) - finished >= 2u)
        return false;

    // A pass without hits (e.g. play/stop to rewind) keeps the previous take.
    if (take.hits.empty())
        return false;

    const auto model = d.messageModel;

    if (model != nullptr && ! model->empty() && sameWindow (windowMs, model->getFeatureSettings().windowMs))
        bbr::reclassifyTake (take, *model);

    lastTake = std::move (take);
    d.lastTakeWindowMs = windowMs;
    return true;
}

void BeatboxProcessor::clearTake()
{
    lastTake = bbr::CaptureTake();
    sendChangeMessage();
}

juce::String BeatboxProcessor::captureFromFile (const juce::File& file)
{
    auto& d = *impl;
    std::vector<float> mono;
    double rate = 0.0;
    const auto error = readAudioFile (file, mono, rate);

    if (error.isNotEmpty())
        return error;

    const auto model = d.messageModel;
    const bool canClassify = model != nullptr && ! model->empty();
    const auto features = canClassify ? model->getFeatureSettings() : currentFeatureSettings();
    const auto hits = bbr::analyzeBuffer (mono.data(), (int64_t) mono.size(), rate, currentDetectorSettings(), features);

    if (hits.empty())
        return "No hits found in \"" + file.getFileName() + "\". Try a lower threshold.";

    bbr::CaptureTake take;
    take.bpm = d.hostBpm.load();
    take.timeSigNum = d.hostTimeSigNum.load();
    take.timeSigDen = d.hostTimeSigDen.load();
    take.startPpq = 0.0;
    take.barStartPpq = 0.0;
    take.hits.reserve (hits.size());

    for (const auto& h : hits)
    {
        bbr::CapturedHit c;
        c.ppq = (double) h.onsetSample / rate * take.bpm / 60.0;
        c.slotId = canClassify ? model->classify (h.features).slotId : bbr::kUnassigned;
        c.peakDb = h.peakDb;
        c.features = h.features;
        take.hits.push_back (c);
    }

    lastTake = std::move (take);
    d.lastTakeWindowMs = features.windowMs;
    sendChangeMessage();
    return {};
}

std::vector<uint8_t> BeatboxProcessor::createTakeMidi (bool fromSongStart) const
{
    int noteForSlot[bbr::kMaxSlots];
    std::fill (std::begin (noteForSlot), std::end (noteForSlot), -1);

    for (const auto& s : impl->slots)
        if (s.id >= 0 && s.id < bbr::kMaxSlots)
            noteForSlot[s.id] = juce::jlimit (0, 127, s.note);

    const double bpm = std::isfinite (lastTake.bpm) && lastTake.bpm > 0.0 ? lastTake.bpm : 120.0;

    bbr::TakeToMidiOptions options;
    options.fromSongStart = fromSongStart;
    options.noteLengthBeats = impl->noteLengthMs() * 0.001 * bpm / 60.0;
    options.dynamicVelocity = impl->dynamicVelocity();
    options.fixedVelocity = impl->fixedVelocity();

    const auto notes = bbr::takeToNotes (lastTake, noteForSlot, impl->messageModel.get(), options);
    const auto trackName = "Beatbox " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M");
    auto bytes = bbr::writeMidiFile (notes, bpm, lastTake.timeSigNum, lastTake.timeSigDen, 960, trackName.toStdString());

    if (! bbr::parseMidiFile (bytes).ok)
        return {};

    return bytes;
}

bool BeatboxProcessor::writeTakeToMidiFile (const juce::File& file, bool fromSongStart) const
{
    if (! hasTake())
        return false;

    const auto bytes = createTakeMidi (fromSongStart);

    if (bytes.empty() || ! file.getParentDirectory().createDirectory().wasOk())
        return false;

    return file.replaceWithData (bytes.data(), bytes.size());
}

juce::File BeatboxProcessor::writeTakeForDrag (bool fromSongStart) const
{
    if (! hasTake())
        return {};

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("BeatboxReplacer")
                         .getChildFile ("drag");

    if (! dir.createDirectory().wasOk())
        return {};

    // Files handed to the host are never deleted right away (it may still be importing them).
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::hours (24.0);

    for (const auto& old : dir.findChildFiles (juce::File::findFiles, false, "*.mid"))
        if (old.getLastModificationTime() < cutoff)
            old.deleteFile();

    const auto stem = "Beatbox Take " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    const auto file = dir.getNonexistentChildFile (stem, ".mid", true);

    if (! writeTakeToMidiFile (file, fromSongStart))
        return {};

    return file;
}

int BeatboxProcessor::getTakeDropBar (bool fromSongStart) const
{
    if (fromSongStart || ! hasTake())
        return 1;

    return bbr::barNumberAt (lastTake.barStartPpq, lastTake.timeSigNum, lastTake.timeSigDen);
}

//==================================================================================================
float BeatboxProcessor::getInputLevelDb() const
{
    const auto now = juce::Time::getMillisecondCounter();

    if (now - impl->lastBlockMs.load (std::memory_order_relaxed) > kMeterStaleMs)
        return -100.0f;

    return juce::Decibels::gainToDecibels (impl->inputLevel.load (std::memory_order_relaxed), -100.0f);
}

bool BeatboxProcessor::isReceivingAudio() const
{
    const auto now = juce::Time::getMillisecondCounter();
    return impl->hasHadSignal.load (std::memory_order_relaxed)
           && now - impl->lastSignalMs.load (std::memory_order_relaxed) < kSignalHoldMs;
}

bool BeatboxProcessor::isHostPlaying() const
{
    return impl->hostPlaying.load (std::memory_order_relaxed);
}

uint32_t BeatboxProcessor::getLiveHitCount (int slotIdOrIgnore) const
{
    int index = -1;

    if (slotIdOrIgnore >= 0 && slotIdOrIgnore < bbr::kMaxSlots)
        index = slotIdOrIgnore;
    else if (slotIdOrIgnore == bbr::kIgnoreSlotId || slotIdOrIgnore == bbr::kMaxSlots)
        index = bbr::kMaxSlots;
    else if (slotIdOrIgnore == bbr::kUnassigned)
        index = bbr::kMaxSlots + 1;

    return index >= 0 ? impl->liveHitCounts[(size_t) index].load (std::memory_order_relaxed) : 0u;
}

int BeatboxProcessor::getLastLiveHitSlot() const
{
    return impl->lastLiveHitSlot.load (std::memory_order_relaxed);
}

//==================================================================================================
void BeatboxProcessor::auditionLearnHit (int hitIndex)
{
    if (hitIndex < 0 || hitIndex >= learnSession.numHits() || ! learnSession.hasAudio())
        return;

    const auto& audio = learnSession.getAudio();
    const double rate = learnSession.getSampleRate();
    const auto total = (int64_t) audio.size();
    const auto onset = learnSession.getHit (hitIndex).onsetSample;
    const auto start = juce::jlimit<int64_t> (0, total, onset - (int64_t) std::llround (kAuditionPreRollSeconds * rate));
    const auto length = juce::jmin<int64_t> (total - start, (int64_t) std::llround (kAuditionHitSeconds * rate));

    if (length > 0)
        auditionAudio (audio.data() + start, (int) length, rate);
}

void BeatboxProcessor::auditionTrainingHit (int trainingIndex)
{
    if (trainingIndex < 0 || trainingIndex >= (int) training.size())
        return;

    const auto& hit = training[(size_t) trainingIndex];
    auditionAudio (hit.snippet.data(), (int) hit.snippet.size(), hit.sampleRate);
}

void BeatboxProcessor::auditionAudio (const float* mono, int numSamples, double sampleRate)
{
    auto& d = *impl;
    const double outRate = d.currentRate.load();

    if (mono == nullptr || numSamples <= 0 || ! (sampleRate > 0.0) || ! (outRate > 0.0))
        return;

    // Linear resampling to the output rate.
    const double step = sampleRate / outRate;
    const auto maxLength = (int) std::ceil (kAuditionBufferSeconds * outRate);
    const int length = juce::jlimit (0, maxLength, (int) std::floor ((double) (numSamples - 1) / step) + 1);
    std::vector<float> resampled ((size_t) length);

    for (int i = 0; i < length; ++i)
    {
        const double pos = (double) i * step;
        const int index = juce::jmin ((int) pos, numSamples - 1);
        const auto frac = (float) (pos - (double) index);
        const float a = mono[index];
        const float b = index + 1 < numSamples ? mono[index + 1] : a;
        const float value = a + frac * (b - a);
        resampled[(size_t) i] = std::isfinite (value) ? value : 0.0f;
    }

    // Short fades so a snippet cut out of a recording doesn't click.
    const int fadeIn = juce::jmin (length / 4, (int) (0.001 * outRate));
    const int fadeOut = juce::jmin (length / 2, (int) (0.015 * outRate));

    for (int i = 0; i < fadeIn; ++i)
        resampled[(size_t) i] *= (float) i / (float) fadeIn;

    for (int i = 0; i < fadeOut; ++i)
        resampled[(size_t) (length - 1 - i)] *= (float) i / (float) fadeOut;

    const juce::SpinLock::ScopedLockType lock (d.auditionLock);
    const int n = juce::jmin (length, (int) d.auditionBuffer.size());
    std::copy (resampled.begin(), resampled.begin() + n, d.auditionBuffer.begin());
    d.auditionLength = n;
    d.auditionPos = 0;
}

//==================================================================================================
juce::String BeatboxProcessor::readAudioFile (const juce::File& file, std::vector<float>& monoOut, double& sampleRateOut)
{
    const auto name = file.getFileName();

    if (! file.existsAsFile())
        return "Couldn't find \"" + file.getFullPathName() + "\".";

    std::unique_ptr<juce::AudioFormatReader> reader (impl->formats.createReaderFor (file));

    if (reader == nullptr)
        return "Couldn't open \"" + name + "\". Use a WAV, AIFF, FLAC or Ogg file.";

    const double rate = reader->sampleRate;

    if (! (rate >= 8000.0 && rate <= 768000.0))
        return "\"" + name + "\" has an unsupported sample rate.";

    const int numChannels = (int) juce::jmin (reader->numChannels, 64u);
    const auto total = juce::jmin (reader->lengthInSamples, (juce::int64) (kMaxFileSeconds * rate));

    if (numChannels <= 0 || total <= 0)
        return "\"" + name + "\" contains no audio.";

    std::vector<float> mono ((size_t) total, 0.0f);
    constexpr int blockLength = 65536;
    juce::AudioBuffer<float> block (numChannels, blockLength);

    for (juce::int64 pos = 0; pos < total; pos += blockLength)
    {
        const int n = (int) juce::jmin ((juce::int64) blockLength, total - pos);

        if (! reader->read (block.getArrayOfWritePointers(), numChannels, pos, n))
            return "Couldn't read \"" + name + "\". The file may be damaged.";

        float* const dest = mono.data() + pos;

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::add (dest, block.getReadPointer (ch), n);

        if (numChannels > 1)
            juce::FloatVectorOperations::multiply (dest, 1.0f / (float) numChannels, n);

        for (int i = 0; i < n; ++i)
            if (! std::isfinite (dest[i]))
                dest[i] = 0.0f;
    }

    monoOut = std::move (mono);
    sampleRateOut = rate;
    return {};
}

//==================================================================================================
std::unique_ptr<juce::XmlElement> BeatboxProcessor::createProfileXml() const
{
    return ProfileIO::toXml (getSlotInfos(), training, currentFeatureSettings().windowMs);
}

void BeatboxProcessor::applyProfile (std::vector<bbr::SlotInfo> slotInfos, std::vector<bbr::TrainingHit> hits,
                                     float windowMs, bool setWindowParameter)
{
    auto& d = *impl;
    std::vector<Slot> newSlots;

    for (const auto& info : slotInfos)
    {
        if (info.id < 0 || info.id >= bbr::kMaxSlots)
            continue;

        Slot slot;
        slot.id = info.id;
        slot.name = juce::String::fromUTF8 (info.name.c_str());
        slot.note = juce::jlimit (0, 127, info.note);
        slot.colour = getSlotColour (info.id);
        newSlots.push_back (slot);
    }

    std::sort (newSlots.begin(), newSlots.end(), [] (const Slot& a, const Slot& b) { return a.id < b.id; });

    {
        const juce::ScopedLock lock (d.profileLock);
        d.slots = std::move (newSlots);
        training = std::move (hits);
        limitTrainingSize (training);
    }

    d.trainingWindowMs = windowMs;

    for (auto& n : d.slotNotes)
        n.store (-1);

    for (const auto& s : d.slots)
        d.slotNotes[(size_t) s.id].store (s.note);

    for (int id = 0; id < bbr::kMaxSlots; ++id)
        if (! hasSlot (id))
            learnSession.forgetSlot (id);

    if (setWindowParameter)
        if (auto* param = apvts.getParameter (ParamIDs::window))
            param->setValueNotifyingHost (param->convertTo0to1 (windowMs));

    rebuildModel();
    sendChangeMessage();
}

bool BeatboxProcessor::exportProfile (const juce::File& file) const
{
    std::unique_ptr<juce::XmlElement> xml;

    {
        const juce::ScopedLock lock (impl->profileLock);
        xml = createProfileXml();
    }

    return xml != nullptr && file.getParentDirectory().createDirectory().wasOk() && xml->writeTo (file);
}

juce::String BeatboxProcessor::importProfile (const juce::File& file)
{
    if (! file.existsAsFile())
        return "Couldn't find \"" + file.getFullPathName() + "\".";

    const auto notAProfile = "\"" + file.getFileName() + "\" isn't a Beatbox Replacer profile.";
    const auto xml = juce::parseXML (file);

    if (xml == nullptr)
        return notAProfile;

    // Accept a bare profile or a whole saved plug-in state.
    const juce::XmlElement* profile = xml->hasTagName (ProfileIO::profileTag) ? xml.get()
                                                                               : xml->getChildByName (ProfileIO::profileTag);
    std::vector<bbr::SlotInfo> slotInfos;
    std::vector<bbr::TrainingHit> hits;
    float windowMs = 20.0f;

    if (profile == nullptr || ! ProfileIO::fromXml (*profile, slotInfos, hits, windowMs))
        return notAProfile;

    applyProfile (std::move (slotInfos), std::move (hits), windowMs, true);
    return {};
}

bool BeatboxProcessor::saveDefaultProfile() const
{
    return exportProfile (getDefaultProfileFile());
}

juce::File BeatboxProcessor::getDefaultProfileFile()
{
   #if JUCE_MAC
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
   #else
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif

    return base.getChildFile ("BeatboxReplacer").getChildFile ("default-profile.xml");
}

//==================================================================================================
void BeatboxProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement root (kStateTag);
    root.setAttribute ("version", kStateVersion);

    if (auto params = apvts.copyState().createXml())
        root.addChildElement (params.release());

    {
        const juce::ScopedLock lock (impl->profileLock);
        root.addChildElement (createProfileXml().release());
    }

    auto* ui = root.createNewChildElement (kUiTag);
    ui->setAttribute ("tab", lastEditorTab);
    ui->setAttribute ("w", editorWidth);
    ui->setAttribute ("h", editorHeight);

    copyXmlToBinary (root, destData);
}

void BeatboxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    const auto paramsType = apvts.state.getType().toString();
    const juce::XmlElement* params = nullptr;
    const juce::XmlElement* profile = nullptr;
    const juce::XmlElement* ui = nullptr;

    if (xml->hasTagName (kStateTag))
    {
        params = xml->getChildByName (paramsType);
        profile = xml->getChildByName (ProfileIO::profileTag);
        ui = xml->getChildByName (kUiTag);
    }
    else if (xml->hasTagName (paramsType))
    {
        params = xml.get(); // parameters only
    }
    else if (xml->hasTagName (ProfileIO::profileTag))
    {
        profile = xml.get();
    }

    if (params != nullptr)
    {
        const auto tree = juce::ValueTree::fromXml (*params);

        if (tree.isValid() && tree.hasType (apvts.state.getType()))
            apvts.replaceState (tree);
    }

    std::vector<bbr::SlotInfo> slotInfos;
    std::vector<bbr::TrainingHit> hits;
    float windowMs = 20.0f;

    if (profile != nullptr && ProfileIO::fromXml (*profile, slotInfos, hits, windowMs))
        applyProfile (std::move (slotInfos), std::move (hits), windowMs, false); // the window comes from the parameters
    else
        rebuildModel();

    if (ui != nullptr)
    {
        lastEditorTab = juce::jlimit (0, 15, ui->getIntAttribute ("tab", lastEditorTab));
        editorWidth = juce::jlimit (320, 8192, ui->getIntAttribute ("w", editorWidth));
        editorHeight = juce::jlimit (240, 8192, ui->getIntAttribute ("h", editorHeight));
    }

    sendChangeMessage();
}

//==================================================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BeatboxProcessor();
}
