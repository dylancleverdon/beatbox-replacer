#include "Version.h"

#include <cctype>
#include <limits>

namespace bbr
{

std::optional<Version> Version::parse (const std::string& text)
{
    const size_t length = text.size();
    size_t pos = 0;

    auto isDigitAt = [&text, length] (size_t p)
    {
        return p < length && text[p] >= '0' && text[p] <= '9';
    };

    while (pos < length && std::isspace ((unsigned char) text[pos]) != 0)
        ++pos;

    if (pos < length && (text[pos] == 'v' || text[pos] == 'V'))
        ++pos;

    if (! isDigitAt (pos))
        return std::nullopt;

    int parts[3] = { 0, 0, 0 };

    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
        {
            if (! (pos < length && text[pos] == '.' && isDigitAt (pos + 1)))
                break;

            ++pos;
        }

        int value = 0;
        constexpr int maxValue = std::numeric_limits<int>::max();

        for (; isDigitAt (pos); ++pos)
        {
            const int digit = text[pos] - '0';
            value = value > (maxValue - digit) / 10 ? maxValue : value * 10 + digit;
        }

        parts[i] = value;
    }

    Version v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    return v;
}

std::string Version::toString() const
{
    return std::to_string (major) + "." + std::to_string (minor) + "." + std::to_string (patch);
}

int compareVersions (const Version& a, const Version& b) noexcept
{
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;

    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;

    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;

    return 0;
}

bool isNewerVersion (const std::string& candidate, const std::string& current)
{
    const auto newer = Version::parse (candidate);

    if (! newer.has_value())
        return false;

    return compareVersions (*newer, Version::parse (current).value_or (Version {})) > 0;
}

} // namespace bbr
