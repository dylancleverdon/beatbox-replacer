// End-to-end test of the real plugin processor (no GUI, no host): teaches it three sounds from a
// synthetic recording, then plays a different synthetic beatbox take through the sidechain and
// checks the live MIDI output, the captured clip, state save/restore and odd block sizes.

#include <JuceHeader.h>

#include "TestSignals.h"
#include "core/Capture.h"
#include "core/MidiFile.h"
#include "plugin/PluginProcessor.h"

#include <cstdio>
#include <map>
#include <thread>

using namespace bbrtest;

namespace
{
int failures = 0;

#define EXPECT(cond, ...)                                                    \
    do {                                                                     \
        if (! (cond))                                                        \
        {                                                                    \
            ++failures;                                                      \
            std::printf ("  FAILED %s:%d: %s ", __FILE__, __LINE__, #cond);  \
            std::printf (__VA_ARGS__);                                       \
            std::printf ("\n");                                              \
        }                                                                    \
    } while (false)

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

int noteFor (Sound s)
{
    switch (s)
    {
        case Sound::kick:  return 36;
        case Sound::snare: return 38;
        case Sound::hat:   return 42;
        case Sound::click:
        case Sound::breath: break;
    }
    return -1;
}

int slotFor (Sound s)
{
    switch (s)
    {
        case Sound::kick:  return 0;
        case Sound::snare: return 1;
        case Sound::hat:   return 2;
        case Sound::click:
        case Sound::breath: break;
    }
    return bbr::kIgnoreSlotId;
}

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = false;
    double startPpq = 8.0; // bar 3 in 4/4
    int64_t samplePos = 0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        const double ppq = startPpq + (double) samplePos / kRate * 2.0; // 120 bpm
        info.setIsPlaying (playing);
        info.setBpm (120.0);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setPpqPosition (ppq);
        info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        info.setTimeInSamples (samplePos);
        info.setTimeInSeconds ((double) samplePos / kRate);
        return info;
    }
};

struct NoteOn
{
    int64_t sample;
    int note;
    int velocity;
};

// Runs mono audio through the sidechain in fixed blocks; returns note-ons with absolute positions.
std::vector<NoteOn> run (BeatboxProcessor& p, const std::vector<float>& audio, FakePlayHead* head,
                         int& noteOffs, bool& outputSilent)
{
    std::vector<NoteOn> notes;
    juce::AudioBuffer<float> buffer (p.getTotalNumInputChannels() > p.getTotalNumOutputChannels()
                                         ? p.getTotalNumInputChannels() : p.getTotalNumOutputChannels(),
                                     kBlock);
    juce::MidiBuffer midi;

    for (size_t pos = 0; pos < audio.size(); pos += kBlock)
    {
        const int n = (int) std::min ((size_t) kBlock, audio.size() - pos);
        buffer.setSize (buffer.getNumChannels(), n, false, false, true);
        buffer.clear();

        auto sidechain = p.getBusBuffer (buffer, true, 1);
        for (int ch = 0; ch < sidechain.getNumChannels(); ++ch)
            sidechain.copyFrom (ch, 0, audio.data() + pos, n);

        midi.clear();
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 100, (juce::uint8) 99), 0); // must not be echoed

        if (head != nullptr)
            head->samplePos = (int64_t) pos;

        p.processBlock (buffer, midi);

        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            EXPECT (meta.samplePosition >= 0 && meta.samplePosition < n, "event at %d in a %d block", meta.samplePosition, n);
            EXPECT (m.getNoteNumber() != 100, "input MIDI was echoed");

            if (m.isNoteOn())
                notes.push_back ({ (int64_t) pos + meta.samplePosition, m.getNoteNumber(), m.getVelocity() });
            else if (m.isNoteOff())
                ++noteOffs;
        }

        auto out = p.getBusBuffer (buffer, false, 0);
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            if (out.getMagnitude (ch, 0, n) > 0.0f)
                outputSilent = false;
    }
    return notes;
}

juce::File writeWav (const std::vector<float>& audio, const juce::String& name)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (kRate)
                                                                               .withNumChannels (1)
                                                                               .withBitsPerSample (24));
    juce::AudioBuffer<float> buf (1, (int) audio.size());
    buf.copyFrom (0, 0, audio.data(), (int) audio.size());
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    return file;
}

void pumpTimers()
{
    for (int i = 0; i < 5; ++i)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (40));
        juce::Timer::callPendingTimersSynchronously();
    }
}

void teach (BeatboxProcessor& p)
{
    const auto training = makeMixedPattern (kRate, 1, 14, 14, 14);
    const auto file = writeWav (training.audio, "bbr-smoke-learn.wav");
    const auto error = p.learnFromFile (file);
    file.deleteFile();
    EXPECT (error.isEmpty(), "learnFromFile: %s", error.toRawUTF8());

    auto& session = p.getLearnSession();
    EXPECT (session.numHits() == (int) training.truth.size(), "learn found %d hits, expected %d",
            session.numHits(), (int) training.truth.size());

    // Assign each group to the slot most of its hits really are.
    std::vector<std::map<int, int>> votes ((size_t) session.numGroups());
    for (int i = 0; i < session.numHits(); ++i)
    {
        const auto onset = session.getHit (i).onsetSample;
        for (const auto& t : training.truth)
            if (std::abs (t.sample - onset) < (int64_t) (0.005 * kRate))
                ++votes[(size_t) session.groupOfHit (i)][slotFor (t.sound)];
    }
    for (int g = 0; g < session.numGroups(); ++g)
    {
        int best = bbr::kUnassigned, bestCount = 0;
        for (auto [slot, count] : votes[(size_t) g])
            if (count > bestCount)
                best = slot, bestCount = count;
        EXPECT (session.getGroupSlot (g) == best, "group %d suggested %d, truth %d", g, session.getGroupSlot (g), best);
        p.setLearnGroupSlot (g, best);
    }

    p.commitLearnSession (false);
    EXPECT (p.getModel() != nullptr && ! p.getModel()->empty(), "no model after commit");
    EXPECT (p.getTrainingCount (0) >= 12 && p.getTrainingCount (1) >= 12 && p.getTrainingCount (2) >= 12,
            "training counts %d %d %d", p.getTrainingCount (0), p.getTrainingCount (1), p.getTrainingCount (2));
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto processor = std::make_unique<BeatboxProcessor>();
    auto& p = *processor;
    EXPECT (p.getBusCount (true) >= 1 && p.getBus (true, p.getBusCount (true) - 1)->getNumberOfChannels() > 0,
            "no sidechain bus");
    p.prepareToPlay (kRate, kBlock);
    teach (p);

    const float windowMs = p.apvts.getRawParameterValue (ParamIDs::window)->load();

    // ---- live MIDI ----------------------------------------------------------------------------
    {
        const auto take = makeMixedPattern (kRate, 2, 12, 12, 12, 0, 0.15, 0.3, { -70.0f, 0.5, true, -3.0f });
        int offs = 0;
        bool silent = true;
        const auto notes = run (p, take.audio, nullptr, offs, silent);

        int correct = 0;
        for (const auto& t : take.truth)
        {
            const auto lo = t.sample, hi = t.sample + (int64_t) ((windowMs + 6.0f) * kRate / 1000.0);
            for (const auto& n : notes)
                if (n.sample >= lo && n.sample <= hi && n.note == noteFor (t.sound))
                {
                    ++correct;
                    break;
                }
        }

        EXPECT (correct >= (int) (0.95 * (double) take.truth.size()), "live: %d/%d correct", correct, (int) take.truth.size());
        EXPECT (notes.size() <= take.truth.size() + 1, "live: %d notes for %d hits", (int) notes.size(), (int) take.truth.size());
        EXPECT (offs == (int) notes.size() || offs + 1 == (int) notes.size(), "note-ons %d vs note-offs %d", (int) notes.size(), offs);
        EXPECT (silent, "audio output not silent");
        std::printf ("live: %d/%d hits -> correct note within %.0f ms\n", correct, (int) take.truth.size(), windowMs + 6.0f);
    }

    // ---- capture -> MIDI file -----------------------------------------------------------------
    {
        FakePlayHead head;
        p.setPlayHead (&head);
        p.setCaptureArmed (true);

        const auto take = makeMixedPattern (kRate, 3, 8, 8, 8);
        head.playing = true;
        int offs = 0;
        bool silent = true;
        run (p, take.audio, &head, offs, silent);

        head.playing = false;
        std::vector<float> stopped ((size_t) kBlock * 4, 0.0f);
        run (p, stopped, &head, offs, silent);
        pumpTimers();

        EXPECT (p.hasTake(), "no take after transport stop");
        EXPECT (p.getTakeDropBar (false) == 3, "drop bar %d", p.getTakeDropBar (false));

        const auto midiFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bbr-smoke.mid");
        EXPECT (p.writeTakeToMidiFile (midiFile, false), "writeTakeToMidiFile failed");

        juce::MemoryBlock data;
        midiFile.loadFileAsData (data);
        midiFile.deleteFile();
        const auto* bytes = static_cast<const uint8_t*> (data.getData());
        const auto parsed = bbr::parseMidiFile (std::vector<uint8_t> (bytes, bytes + data.getSize()));
        EXPECT (parsed.ok && parsed.ppq == 960 && parsed.bpm <= 0.0, "bad midi file (no tempo event expected)");

        int matched = 0;
        for (const auto& t : take.truth)
        {
            const double tick = (8.0 + (double) t.sample / kRate * 2.0 - 8.0) * 960.0;
            for (const auto& n : parsed.notes)
                if (n.note == noteFor (t.sound) && std::abs ((double) n.startTick - tick) <= 20.0)
                {
                    ++matched;
                    break;
                }
        }
        EXPECT (matched >= (int) (0.95 * (double) take.truth.size()), "capture: %d/%d notes placed", matched, (int) take.truth.size());
        EXPECT (parsed.notes.size() <= take.truth.size(), "capture: %d notes for %d hits", (int) parsed.notes.size(), (int) take.truth.size());
        std::printf ("capture: %d/%d notes within 10 ms in the .mid\n", matched, (int) take.truth.size());

        const auto dragFile = p.writeTakeForDrag (false);
        EXPECT (dragFile.existsAsFile() && dragFile.getSize() > 20, "drag file not written");
        p.setCaptureArmed (false);
        p.setPlayHead (nullptr);
    }

    // ---- state round trip ---------------------------------------------------------------------
    {
        juce::MemoryBlock state;
        p.getStateInformation (state);

        BeatboxProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        EXPECT (restored.getTrainingHits().size() == p.getTrainingHits().size(), "restored %d of %d training hits",
                (int) restored.getTrainingHits().size(), (int) p.getTrainingHits().size());
        EXPECT (restored.getSlots().size() == p.getSlots().size(), "slot count");
        EXPECT (restored.getModel() != nullptr && ! restored.getModel()->empty(), "restored model empty");

        restored.prepareToPlay (kRate, kBlock);
        const auto take = makeMixedPattern (kRate, 4, 5, 5, 5);
        int offs = 0;
        bool silent = true;
        const auto notes = run (restored, take.audio, nullptr, offs, silent);
        EXPECT ((int) notes.size() >= 14, "restored instance produced %d notes", (int) notes.size());

        // garbage state must not crash
        const char junk[] = "<BeatboxReplacerState version=\"1\"><Profile><Hit slot=\"99\">%%%</Hit></Profile>";
        restored.setStateInformation (junk, (int) sizeof (junk));
        restored.setStateInformation (nullptr, 0);
    }

    // ---- blocks bigger than announced, bypass ------------------------------------------------
    {
        juce::AudioBuffer<float> big (4, 4096);
        big.clear();
        juce::MidiBuffer midi;
        p.processBlock (big, midi);
        p.processBlockBypassed (big, midi);
        p.releaseResources();
    }

    std::printf (failures == 0 ? "plugin smoke test passed\n" : "plugin smoke test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
