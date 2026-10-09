#include "Types.h"

#include <cctype>

namespace bbr
{

namespace
{
    const char* const kSharpNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // Pitch class of the natural notes A .. G.
    const int kLetterPitchClass[7] = { 9, 11, 0, 2, 4, 5, 7 };

    bool isSpace (char c) noexcept
    {
        return std::isspace ((unsigned char) c) != 0;
    }
}

std::string noteName (int midiNote)
{
    // Ableton: 60 = C3, so octave = floor (note / 12) - 2.
    const long long note = midiNote;
    const long long pitchClass = ((note % 12) + 12) % 12;
    const long long octave = (note - pitchClass) / 12 - 2;
    return std::string (kSharpNames[pitchClass]) + std::to_string (octave);
}

int parseNoteName (const std::string& text)
{
    size_t pos = 0;
    size_t end = text.size();

    while (pos < end && isSpace (text[pos]))
        ++pos;

    while (end > pos && isSpace (text[end - 1]))
        --end;

    if (pos >= end)
        return -1;

    const char letter = (char) std::toupper ((unsigned char) text[pos++]);

    if (letter < 'A' || letter > 'G')
        return -1;

    int pitch = kLetterPitchClass[letter - 'A'];

    if (pos < end && text[pos] == '#')
    {
        ++pitch;
        ++pos;
    }
    else if (pos < end && (text[pos] == 'b' || text[pos] == 'B'))
    {
        --pitch;
        ++pos;
    }

    bool negative = false;

    if (pos < end && text[pos] == '-')
    {
        negative = true;
        ++pos;
    }

    if (pos >= end)
        return -1;

    int octave = 0;

    for (; pos < end; ++pos)
    {
        const char c = text[pos];

        if (c < '0' || c > '9')
            return -1;

        octave = octave * 10 + (c - '0');

        if (octave > 100)
            return -1;
    }

    if (negative)
        octave = -octave;

    const int note = (octave + 2) * 12 + pitch;
    return (note >= 0 && note <= 127) ? note : -1;
}

} // namespace bbr
