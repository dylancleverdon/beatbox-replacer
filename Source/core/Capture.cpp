#include "Capture.h"

#include "Analysis.h"

#include <cmath>
#include <limits>

namespace bbr
{

double quarterNotesPerBar (int timeSigNum, int timeSigDen) noexcept
{
    if (timeSigNum <= 0 || timeSigDen <= 0)
        return 4.0;

    return (double) timeSigNum * 4.0 / (double) timeSigDen;
}

int barNumberAt (double ppq, int timeSigNum, int timeSigDen) noexcept
{
    if (! std::isfinite (ppq) || ppq <= 0.0)
        return 1;

    // The epsilon keeps a host position a hair before a bar line (7.9999999999) in the new bar.
    const double bar = std::floor (ppq / quarterNotesPerBar (timeSigNum, timeSigDen) + 1.0e-9) + 1.0;

    if (bar >= (double) std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();

    return bar < 1.0 ? 1 : (int) bar;
}

void reclassifyTake (CaptureTake& take, const ClassifierModel& model)
{
    if (model.empty())
        return;

    for (auto& hit : take.hits)
        hit.slotId = model.classify (hit.features).slotId;
}

std::vector<MidiNote> takeToNotes (const CaptureTake& take, const int (&noteForSlot)[kMaxSlots],
                                   const ClassifierModel* model, const TakeToMidiOptions& options)
{
    std::vector<MidiNote> notes;
    notes.reserve (take.hits.size());

    const double clipStart = options.fromSongStart ? 0.0 : take.barStartPpq;

    for (const auto& hit : take.hits)
    {
        if (hit.slotId < 0 || hit.slotId >= kMaxSlots)
            continue;

        const int note = noteForSlot[hit.slotId];
        const double start = hit.ppq - clipStart;

        if (note < 0 || note > 127 || ! std::isfinite (start) || start < 0.0)
            continue;

        const float referenceDb = model != nullptr ? model->getMedianPeakDb (hit.slotId)
                                                   : std::numeric_limits<float>::quiet_NaN();

        MidiNote n;
        n.startBeats = start;
        n.lengthBeats = options.noteLengthBeats;
        n.note = note;
        n.velocity = velocityFor (hit.peakDb, referenceDb, options.dynamicVelocity, options.fixedVelocity);
        n.channel = 0;
        notes.push_back (n);
    }

    return notes;
}

} // namespace bbr
