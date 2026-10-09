#include "MidiFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bbr
{

namespace
{
// Largest value a 4-byte variable-length quantity can hold; also our limit for absolute ticks.
constexpr int64_t kMaxTick = 0x0FFFFFFF;

void writeVarLen (std::vector<uint8_t>& out, uint32_t value)
{
    value = std::min (value, (uint32_t) kMaxTick);
    uint8_t buffer[4];
    int count = 0;
    buffer[count++] = (uint8_t) (value & 0x7Fu);

    while ((value >>= 7) != 0)
        buffer[count++] = (uint8_t) ((value & 0x7Fu) | 0x80u);

    while (count > 0)
        out.push_back (buffer[--count]);
}

void writeBigEndian (std::vector<uint8_t>& out, uint32_t value, int numBytes)
{
    for (int i = numBytes - 1; i >= 0; --i)
        out.push_back ((uint8_t) ((value >> (8 * i)) & 0xFFu));
}

void writeMetaHeader (std::vector<uint8_t>& track, uint8_t type, size_t length)
{
    track.push_back (0xFF);
    track.push_back (type);
    writeVarLen (track, (uint32_t) length);
}

struct NoteEvent
{
    int64_t tick;
    int isOn;      // 0 = note-off, 1 = note-on: offs sort first at equal ticks
    size_t order;  // index in start order
    uint8_t status, key, velocity;
};

// Bounds-checked big-endian reader. Any read past the end sets failed() and returns zeros.
class ByteReader
{
public:
    ByteReader (const uint8_t* bytesToRead, size_t numBytes) noexcept : bytes (bytesToRead), length (numBytes) {}

    bool failed() const noexcept { return error; }
    bool atEnd() const noexcept { return pos >= length; }
    size_t remaining() const noexcept { return length - pos; }

    uint8_t peek() noexcept
    {
        if (pos >= length)
        {
            error = true;
            return 0;
        }

        return bytes[pos];
    }

    uint8_t byte() noexcept
    {
        const uint8_t b = peek();

        if (! error)
            ++pos;

        return b;
    }

    uint32_t bigEndian (int numBytes) noexcept
    {
        uint32_t v = 0;

        for (int i = 0; i < numBytes; ++i)
            v = (v << 8) | (uint32_t) byte();

        return error ? 0 : v;
    }

    uint32_t varLen() noexcept
    {
        uint32_t v = 0;

        for (int i = 0; i < 4; ++i)
        {
            const uint8_t b = byte();

            if (error)
                return 0;

            v = (v << 7) | (uint32_t) (b & 0x7Fu);

            if ((b & 0x80u) == 0)
                return v;
        }

        error = true; // more than 4 bytes
        return 0;
    }

    // Returns a pointer to the next n bytes and skips them, or nullptr (and fails) if there
    // aren't that many.
    const uint8_t* take (size_t n) noexcept
    {
        if (error || n > length - pos)
        {
            error = true;
            return nullptr;
        }

        const uint8_t* p = bytes + pos;
        pos += n;
        return p;
    }

private:
    const uint8_t* bytes;
    size_t length;
    size_t pos = 0;
    bool error = false;
};

struct ParseState
{
    bool haveTempo = false, haveTimeSig = false, haveName = false;
};

// Parses one MTrk chunk body. Returns false on malformed data or a missing end-of-track event.
bool parseTrack (const uint8_t* data, size_t size, ParsedMidiFile& out, ParseState& state)
{
    ByteReader r (data, size);
    int64_t tick = 0;
    int runningStatus = 0;
    std::vector<size_t> open; // indices into out.notes still waiting for their note-off

    while (! r.atEnd())
    {
        tick += (int64_t) r.varLen();
        const uint8_t first = r.peek();

        if (r.failed())
            return false;

        int status = 0;

        if ((first & 0x80u) != 0)
        {
            status = first;
            r.byte();
        }
        else if (runningStatus != 0)
        {
            status = runningStatus; // running status: `first` is already the first data byte
        }
        else
        {
            return false;
        }

        if (status == 0xFF)
        {
            runningStatus = 0;
            const uint8_t type = r.byte();
            const uint32_t len = r.varLen();
            const uint8_t* payload = r.take (len);

            if (r.failed())
                return false;

            if (type == 0x2F)
            {
                for (size_t idx : open)
                    out.notes[idx].endTick = tick;

                return true;
            }

            if (type == 0x51 && len >= 3 && ! state.haveTempo)
            {
                const uint32_t microsPerQuarter = ((uint32_t) payload[0] << 16) | ((uint32_t) payload[1] << 8)
                                                  | (uint32_t) payload[2];

                if (microsPerQuarter > 0)
                {
                    out.bpm = 60.0e6 / (double) microsPerQuarter;
                    state.haveTempo = true;
                }
            }
            else if (type == 0x58 && len >= 2 && ! state.haveTimeSig)
            {
                out.timeSigNum = payload[0];
                out.timeSigDen = payload[1] < 31 ? (1 << payload[1]) : 0;
                state.haveTimeSig = true;
            }
            else if (type == 0x03 && ! state.haveName)
            {
                out.trackName.assign (reinterpret_cast<const char*> (payload), len);
                state.haveName = true;
            }

            continue;
        }

        if (status == 0xF0 || status == 0xF7)
        {
            runningStatus = 0;
            r.take (r.varLen());

            if (r.failed())
                return false;

            continue;
        }

        if (status > 0xF0)
            return false; // system common / real-time messages can't appear in a file

        runningStatus = status;
        const int type = status & 0xF0;
        const int channel = status & 0x0F;
        const uint8_t d1 = r.byte();
        uint8_t d2 = 0;

        if (type != 0xC0 && type != 0xD0)
            d2 = r.byte();

        if (r.failed() || (d1 & 0x80u) != 0 || (d2 & 0x80u) != 0)
            return false;

        if (type == 0x90 && d2 > 0)
        {
            ParsedMidiNote n;
            n.startTick = tick;
            n.endTick = tick;
            n.note = d1;
            n.velocity = d2;
            n.channel = channel;
            open.push_back (out.notes.size());
            out.notes.push_back (n);
        }
        else if (type == 0x80 || type == 0x90)
        {
            // Close the oldest sounding note with this key and channel.
            for (auto it = open.begin(); it != open.end(); ++it)
            {
                ParsedMidiNote& n = out.notes[*it];

                if (n.note == d1 && n.channel == channel)
                {
                    n.endTick = tick;
                    open.erase (it);
                    break;
                }
            }
        }
    }

    return false; // no end-of-track event
}
} // namespace

std::vector<uint8_t> writeMidiFile (const std::vector<MidiNote>& notes, double bpm,
                                    int timeSigNum, int timeSigDen, int ppq,
                                    const std::string& trackName, bool writeTempoAndTimeSignature)
{
    const int ticksPerQuarter = (ppq > 0 && ppq <= 0x7FFF) ? ppq : 960;
    const double tempoBpm = (std::isfinite (bpm) && bpm > 0.0) ? bpm : 120.0;

    // The file stores the denominator as a power of two; anything unrepresentable becomes 4/4.
    int num = 4, den = 4, denPow = 2;

    if (timeSigNum > 0 && timeSigNum <= 255)
    {
        for (int p = 0; p <= 7; ++p)
        {
            if ((1 << p) == timeSigDen)
            {
                num = timeSigNum;
                den = timeSigDen;
                denPow = p;
            }
        }
    }

    // ---- note events --------------------------------------------------------------------
    std::vector<size_t> byStart;
    byStart.reserve (notes.size());

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const MidiNote& n = notes[i];

        if (std::isfinite (n.startBeats) && std::isfinite (n.lengthBeats) && n.note >= 0 && n.note <= 127)
            byStart.push_back (i);
    }

    std::stable_sort (byStart.begin(), byStart.end(), [&notes] (size_t a, size_t b)
    {
        return notes[a].startBeats < notes[b].startBeats;
    });

    std::vector<NoteEvent> events;
    events.reserve (byStart.size() * 2);
    int64_t lastTick = 0;

    for (size_t order = 0; order < byStart.size(); ++order)
    {
        const MidiNote& n = notes[byStart[order]];
        const double startBeats = std::max (0.0, n.startBeats);
        const double endTicksExact = (startBeats + std::max (0.0, n.lengthBeats)) * (double) ticksPerQuarter;

        if (endTicksExact >= (double) (kMaxTick - 1))
            continue;

        const int64_t on = (int64_t) std::llround (startBeats * (double) ticksPerQuarter);
        const int64_t off = std::max (on + 1, (int64_t) std::llround (endTicksExact));
        const uint8_t channel = (uint8_t) std::clamp (n.channel, 0, 15);
        const uint8_t key = (uint8_t) n.note;
        const uint8_t velocity = (uint8_t) std::clamp (n.velocity, 1, 127);

        events.push_back ({ on, 1, order, (uint8_t) (0x90u | channel), key, velocity });
        events.push_back ({ off, 0, order, (uint8_t) (0x80u | channel), key, 0x40 });
        lastTick = std::max (lastTick, off);
    }

    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.tick != b.tick)
            return a.tick < b.tick;

        if (a.isOn != b.isOn)
            return a.isOn < b.isOn;

        return a.order < b.order;
    });

    // End of track: the end of the bar containing the last note-off (at least one bar).
    const double barTicks = (double) ticksPerQuarter * (double) num * 4.0 / (double) den;
    const double bars = std::max (1.0, std::ceil ((double) lastTick / barTicks - 1.0e-9));
    const int64_t barEnd = (int64_t) std::min ((double) kMaxTick, std::ceil (bars * barTicks - 1.0e-9));
    const int64_t endTick = std::max (lastTick, barEnd);

    // ---- track chunk --------------------------------------------------------------------
    std::vector<uint8_t> track;
    track.reserve (64 + events.size() * 8 + trackName.size());

    const size_t nameLength = std::min (trackName.size(), (size_t) kMaxTick);
    track.push_back (0x00);
    writeMetaHeader (track, 0x03, nameLength);

    for (size_t i = 0; i < nameLength; ++i)
        track.push_back ((uint8_t) trackName[i]);

    if (writeTempoAndTimeSignature)
    {
        const int64_t micros = std::clamp ((int64_t) std::llround (60.0e6 / tempoBpm), (int64_t) 1, (int64_t) 0xFFFFFF);
        track.push_back (0x00);
        writeMetaHeader (track, 0x51, 3);
        writeBigEndian (track, (uint32_t) micros, 3);

        track.push_back (0x00);
        writeMetaHeader (track, 0x58, 4);
        track.push_back ((uint8_t) num);
        track.push_back ((uint8_t) denPow);
        track.push_back (24); // MIDI clocks per metronome click
        track.push_back (8);  // 32nd notes per quarter note
    }

    int64_t previousTick = 0;

    for (const auto& e : events)
    {
        writeVarLen (track, (uint32_t) (e.tick - previousTick));
        track.push_back (e.status);
        track.push_back (e.key);
        track.push_back (e.velocity);
        previousTick = e.tick;
    }

    writeVarLen (track, (uint32_t) (endTick - previousTick));
    writeMetaHeader (track, 0x2F, 0);

    // ---- file ---------------------------------------------------------------------------
    std::vector<uint8_t> file;
    file.reserve (22 + track.size());

    for (char c : { 'M', 'T', 'h', 'd' })
        file.push_back ((uint8_t) c);

    writeBigEndian (file, 6, 4);
    writeBigEndian (file, 0, 2); // format 0
    writeBigEndian (file, 1, 2); // one track
    writeBigEndian (file, (uint32_t) ticksPerQuarter, 2);

    for (char c : { 'M', 'T', 'r', 'k' })
        file.push_back ((uint8_t) c);

    writeBigEndian (file, (uint32_t) track.size(), 4);
    file.insert (file.end(), track.begin(), track.end());
    return file;
}

ParsedMidiFile parseMidiFile (const std::vector<uint8_t>& bytes)
{
    ParsedMidiFile result;
    ByteReader r (bytes.data(), bytes.size());

    const uint8_t* headerId = r.take (4);
    const uint32_t headerLength = r.bigEndian (4);

    if (r.failed() || std::memcmp (headerId, "MThd", 4) != 0 || headerLength < 6)
        return result;

    const uint8_t* header = r.take (headerLength);

    if (r.failed())
        return result;

    result.format = (header[0] << 8) | header[1];
    result.numTracks = (header[2] << 8) | header[3];
    const int division = (header[4] << 8) | header[5];

    if ((division & 0x8000) != 0 || division == 0)
        return result; // SMPTE time division is not supported

    result.ppq = division;

    ParseState state;
    int tracksFound = 0;

    while (! r.atEnd())
    {
        // Tolerate a few bytes of padding after the last track.
        if (tracksFound >= result.numTracks && r.remaining() < 8)
            break;

        const uint8_t* chunkId = r.take (4);
        const uint32_t chunkLength = r.bigEndian (4);
        const uint8_t* chunk = r.take (chunkLength);

        if (r.failed())
            return result;

        if (std::memcmp (chunkId, "MTrk", 4) == 0)
        {
            if (! parseTrack (chunk, chunkLength, result, state))
                return result;

            ++tracksFound;
        }
    }

    if (tracksFound < result.numTracks)
        return result;

    std::stable_sort (result.notes.begin(), result.notes.end(), [] (const ParsedMidiNote& a, const ParsedMidiNote& b)
    {
        return a.startTick < b.startTick;
    });

    result.ok = true;
    return result;
}

} // namespace bbr
