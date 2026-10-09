#pragma once

#include <JuceHeader.h>

#include "../core/Capture.h"
#include "../core/Classifier.h"
#include "../core/LearnSession.h"
#include "../core/LiveEngine.h"
#include "../core/Types.h"

#include <array>
#include <atomic>
#include <limits>
#include <memory>

// Parameter IDs (AudioProcessorValueTreeState).
namespace ParamIDs
{
    inline constexpr const char* live = "live";            // bool, default true: send live MIDI
    inline constexpr const char* threshold = "threshold";  // float dB, -70 .. -10, default -48
    inline constexpr const char* rise = "rise";            // float dB, 3 .. 24, default 9
    inline constexpr const char* minGap = "minGap";        // float ms, 20 .. 300, default 60
    inline constexpr const char* window = "window";        // float ms, 8 .. 60, default 20 (not automatable)
    inline constexpr const char* dynamic = "dynamic";      // bool, default true: velocity follows loudness
    inline constexpr const char* fixedVelocity = "fixedVel"; // int 1 .. 127, default 100
    inline constexpr const char* noteLength = "noteLen";   // float ms, 10 .. 500, default 60
}

// The plugin: an instrument with a stereo sidechain input that listens to beatboxing and turns
// each hit into a MIDI note for the slot (Kick / Snare / Hi-hat / ...) it sounds most like.
//
// Threading rules
//   * Audio thread: processBlock only. No allocation, no blocking locks, no logging.
//     It may take juce::SpinLock locks only with ScopedTryLock (and must cope with failure).
//   * Everything public below except processBlock is MESSAGE THREAD ONLY unless marked
//     "any thread".
//   * The processor runs a juce::Timer (30 Hz) on the message thread that notices transport
//     changes reported by the audio thread (learn/capture start/stop), finishes recordings,
//     drops retired models, and calls sendChangeMessage() whenever UI-visible state changes.
class BeatboxProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster,
                         private juce::Timer,
                         private juce::AudioProcessorValueTreeState::Listener
{
public:
    BeatboxProcessor();
    ~BeatboxProcessor() override;

    // ---- juce::AudioProcessor -------------------------------------------------------------
    // Buses (default build): input 0 "Input" (main; disabled, mono or stereo -- Live does not feed
    // it on an instrument), input 1 "Sidechain" (aux; mono or stereo, NOT disabled: when the
    // processor refuses a disabled sidechain, JUCE's VST3 wrapper hands it zeros when the host
    // deactivates the bus), output 0 "Output" (mono or stereo; Live wants an audio output).
    // With -DBBR_AUX_ONLY_SIDECHAIN=1 the only input is the sidechain at index 0 and the
    // VST3ClientExtensions override getPluginHasMainInput() returns false, so it is still kAux.
    // processBlock: clear incoming MIDI first (the VST3 wrapper shares one MidiBuffer for in and
    // out); copy the analysis input (sidechain via getBusBuffer if it has channels, else the main
    // input, else silence; channels averaged to mono) into a member buffer BEFORE touching the
    // output (buffers are in place); then clear every output channel and add audition audio.
    // MIDI events are clamped to [0, numSamples - 1]; note-offs due in a later block carry over.
    // Never define JucePlugin_PreferredChannelConfigurations (it hides the sidechain).
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Clears audio + incoming MIDI and sends note-offs for any held notes.
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    // Creates BeatboxEditor (PluginEditor.h).
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    // Infinite tail: Live may stop processing an instrument that gets no MIDI and outputs
    // silence; the plugin must keep listening to the sidechain.
    double getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // State = APVTS parameters + profile (slots + training) + UI state, as XML.
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ---- Slots -------------------------------------------------------------------------------
    struct Slot
    {
        int id = 0;              // [0, kMaxSlots)
        juce::String name;
        int note = 36;
        juce::Colour colour;     // fixed per id, see getSlotColour()
    };

    // Default slots for a fresh instance: Kick C1 (36), Snare D1 (38), Hi-hat F#1 (42).
    std::vector<Slot> getSlots() const;                 // sorted by id
    std::vector<bbr::SlotInfo> getSlotInfos() const;
    bool hasSlot (int id) const;
    juce::String getSlotName (int id) const;            // "Ignore" for kIgnoreSlotId, "—" for kUnassigned
    static juce::Colour getSlotColour (int id);         // fixed palette; grey for ignore/unassigned

    // Adds a slot with the lowest free id. Name/note default from a list of common drum sounds
    // (Open hat A#1 46, Clap D#1 39, Rim C#1 37, Tom low A1 45, Tom high C2 48, Crash C#2 49)
    // skipping notes already in use. Returns the id, or -1 when kMaxSlots are in use.
    int addSlot();
    // Removes the slot, its training hits, and any learn-session assignment pointing at it.
    void removeSlot (int id);
    void setSlotName (int id, const juce::String& name);
    void setSlotNote (int id, int note);

    // ---- Learn -------------------------------------------------------------------------------
    enum class LearnState
    {
        idle,       // nothing recorded or session shown
        armed,      // waiting for the host transport to start (or for audio, if the host has no transport)
        recording,  // recording sidechain audio
        ready       // recording analysed; getLearnSession() has hits and groups
    };

    // Arms recording. If the host reports a transport, recording starts when it starts playing
    // and stops (and gets analysed) when it stops. Without transport info, recording starts
    // immediately and stops on stopLearn(). Max length kMaxLearnSeconds.
    void startLearn();
    // Stops recording (if any) and analyses what was recorded.
    void stopLearn();
    // Loads an audio file (wav/aiff/flac/ogg/mp3 via AudioFormatManager::registerBasicFormats),
    // mixes to mono and analyses it as a learn recording. Returns an error message or empty.
    juce::String learnFromFile (const juce::File& file);
    LearnState getLearnState() const;
    double getLearnRecordedSeconds() const;             // any thread
    static constexpr double kMaxLearnSeconds = 120.0;

    bbr::LearnSession& getLearnSession() { return learnSession; }
    // Re-cluster the session into k groups.
    void reclusterLearnSession (int k);
    void setLearnGroupSlot (int group, int slotId);
    void setLearnHitSlot (int hit, int slotId);         // kUnassigned clears the override
    // Adds the session's labelled hits to the training set (or replaces the training set) and
    // rebuilds the model. Clears the session afterwards (state -> idle).
    void commitLearnSession (bool replaceExisting);
    void discardLearnSession();

    // ---- Training / model --------------------------------------------------------------------
    const std::vector<bbr::TrainingHit>& getTrainingHits() const { return training; }
    int getTrainingCount (int slotId) const;            // slot id or kIgnoreSlotId
    void clearTraining();
    // Removes training hits for one slot id (or kIgnoreSlotId).
    void clearTrainingFor (int slotId);
    std::shared_ptr<const bbr::ClassifierModel> getModel() const; // message thread snapshot

    // ---- Capture (drag-a-clip workflow) ------------------------------------------------------
    // While armed, every pass of the host transport records a new take of classified hits with
    // their exact onset positions. When the transport stops, the take becomes available.
    void setCaptureArmed (bool shouldBeArmed);
    bool isCaptureArmed() const;
    bool isCaptureRecording() const;                    // armed and transport playing
    // Latest finished take (re-classified with the current model). Empty hits if none.
    const bbr::CaptureTake& getLastTake() const { return lastTake; }
    bool hasTake() const { return ! lastTake.hits.empty(); }
    void clearTake();
    // Converts an audio file straight into a take (bpm/time signature from the host if known,
    // else 120 bpm 4/4; positions measured from the file start, i.e. bar 1).
    juce::String captureFromFile (const juce::File& file);
    // Writes the take as a .mid file (writeMidiFile with no tempo/time-signature events, track
    // name "Beatbox <date time>"), via MemoryOutputStream + File::replaceWithData so the file is
    // complete and closed when this returns. fromSongStart: see bbr::TakeToMidiOptions.
    bool writeTakeToMidiFile (const juce::File& file, bool fromSongStart) const;
    // Writes the take to a NEW uniquely named file under <temp>/BeatboxReplacer/drag (sweeping
    // files there older than 24 h first) for dragging into the DAW. Returns {} on failure.
    juce::File writeTakeForDrag (bool fromSongStart) const;
    // Bar number the clip should be dropped at (1 when fromSongStart).
    int getTakeDropBar (bool fromSongStart) const;
    static constexpr int kMaxCaptureHits = 8192;

    // ---- Monitoring / feedback (any thread) --------------------------------------------------
    float getInputLevelDb() const;                      // smoothed peak of the analysed input
    bool isReceivingAudio() const;                      // input above -60 dBFS within the last 2 s
    bool isHostPlaying() const;
    // Increments every time a live hit for slot id fires (index kMaxSlots = ignored hits).
    uint32_t getLiveHitCount (int slotIdOrIgnore) const;
    int getLastLiveHitSlot() const;                     // slot id / kIgnoreSlotId / kUnassigned

    // ---- Audition (plays through the plugin's audio output) ----------------------------------
    void auditionLearnHit (int hitIndex);               // from the learn recording
    void auditionTrainingHit (int trainingIndex);
    void auditionAudio (const float* mono, int numSamples, double sampleRate);

    // ---- Profiles ----------------------------------------------------------------------------
    // A profile = slots + training hits (+ window setting). Same XML as inside the plugin state.
    bool exportProfile (const juce::File& file) const;
    juce::String importProfile (const juce::File& file); // error message or empty
    bool saveDefaultProfile() const;                     // loaded by new instances
    static juce::File getDefaultProfileFile();           // <user app data>/BeatboxReplacer/default-profile.xml

    // ---- UI state persisted with the project -------------------------------------------------
    int lastEditorTab = 0;
    int editorWidth = 960, editorHeight = 640;

private:
    // implementation details intentionally left to PluginProcessor.cpp; add what you need.
    struct Impl;
    std::unique_ptr<Impl> impl;

    bbr::LearnSession learnSession;
    std::vector<bbr::TrainingHit> training;
    bbr::CaptureTake lastTake;

    void timerCallback() override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatboxProcessor)
};
