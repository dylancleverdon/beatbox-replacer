#pragma once

#include <optional>
#include <string>

namespace bbr
{

struct Version
{
    int major = 0, minor = 0, patch = 0;

    // Accepts "1.2.3", "v1.2.3", "V1.2", "1" (missing parts are 0), surrounding whitespace, and
    // ignores any suffix after the numeric part ("1.2.3-beta" -> 1.2.3). Returns nullopt if
    // there is no leading number.
    static std::optional<Version> parse (const std::string& text);

    std::string toString() const; // "1.2.3"
};

// <0 if a < b, 0 if equal, >0 if a > b.
int compareVersions (const Version& a, const Version& b) noexcept;

// True if `candidate` parses and is strictly newer than `current` (an unparsable current
// version counts as 0.0.0).
bool isNewerVersion (const std::string& candidate, const std::string& current);

} // namespace bbr
