#pragma once

#include "Classifier.h"
#include "MidiFile.h"
#include "Types.h"

#include <vector>

namespace bbr
{

// A hit recorded while "Capture" was armed and the host transport was playing.
struct CapturedHit
{
    double ppq = 0.0;          // host timeline position of the ONSET (not of the delayed live note)
    int slotId = kUnassigned;  // classification at capture time
    float peakDb = -120.0f;
    Features features {};      // kept so the take can be re-classified after re-training
};

// One pass of the transport while Capture was armed.
struct CaptureTake
{
    double startPpq = 0.0;     // host position when recording started
    double barStartPpq = 0.0;  // start of the bar containing startPpq
    double bpm = 120.0;
    int timeSigNum = 4, timeSigDen = 4;
    std::vector<CapturedHit> hits;
};

// Beats per bar for a time signature, in quarter notes (6/8 -> 3.0).
double quarterNotesPerBar (int timeSigNum, int timeSigDen) noexcept;

// 1-based bar number in which a ppq position falls, assuming a constant time signature
// from the start of the song (bar 1 starts at ppq 0).
int barNumberAt (double ppq, int timeSigNum, int timeSigDen) noexcept;

// Re-classifies every hit of the take with `model` (no-op if model is null or empty).
void reclassifyTake (CaptureTake& take, const ClassifierModel& model);

struct TakeToMidiOptions
{
    // false: the clip starts at the bar where capture started (drop it at that bar);
    // true:  the clip starts at the song start (drop it at bar 1).
    bool fromSongStart = false;
    double noteLengthBeats = 0.25;
    bool dynamicVelocity = true;
    int fixedVelocity = 100;
};

// Builds MIDI notes for a take. noteForSlot[slotId] gives each slot's note (-1 = slot unused);
// hits whose slot is kIgnoreSlotId, kUnassigned, or maps to -1 are dropped. Velocity comes from
// velocityFor(hit.peakDb, model ? model->getMedianPeakDb(slot) : NaN, ...).
// startBeats = hit.ppq - clipStart, where clipStart is take.barStartPpq or 0. Hits before
// clipStart are dropped.
std::vector<MidiNote> takeToNotes (const CaptureTake& take, const int (&noteForSlot)[kMaxSlots],
                                   const ClassifierModel* model, const TakeToMidiOptions& options);

} // namespace bbr
