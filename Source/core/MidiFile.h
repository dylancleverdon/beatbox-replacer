#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bbr
{

struct MidiNote
{
    double startBeats = 0.0;   // quarter notes from the start of the file (>= 0)
    double lengthBeats = 0.25;
    int note = 36;             // 0..127
    int velocity = 100;        // 1..127
    int channel = 0;           // 0..15
};

// Serialises notes to a Standard MIDI File (format 0, one track) with the given PPQ.
// Track contents, in order at tick 0: track name meta event, tempo meta event (from bpm),
// time signature meta event; then note on/off pairs (note-off as 0x80 with velocity 0x40).
// Notes are sorted by start; at equal ticks note-offs are written before note-ons, so a
// re-triggered note is never cut short. Ticks are rounded to the nearest integer; a note's
// length is at least 1 tick. Ends with an end-of-track meta event at
// max(last note-off, ceil to the next whole bar of timeSigNum/timeSigDen).
std::vector<uint8_t> writeMidiFile (const std::vector<MidiNote>& notes, double bpm,
                                    int timeSigNum = 4, int timeSigDen = 4, int ppq = 960,
                                    const std::string& trackName = "Beatbox");

// Minimal parser used by tests and by the plugin to sanity-check written files.
struct ParsedMidiNote
{
    int64_t startTick = 0, endTick = 0;
    int note = 0, velocity = 0, channel = 0;
};

struct ParsedMidiFile
{
    bool ok = false;
    int format = 0, numTracks = 0, ppq = 0;
    double bpm = 0.0;           // from the first tempo event, 0 if none
    int timeSigNum = 0, timeSigDen = 0;
    std::string trackName;
    std::vector<ParsedMidiNote> notes; // sorted by startTick
};

ParsedMidiFile parseMidiFile (const std::vector<uint8_t>& bytes);

} // namespace bbr
