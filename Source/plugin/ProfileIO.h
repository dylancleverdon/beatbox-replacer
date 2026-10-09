#pragma once

#include <JuceHeader.h>

#include "../core/Types.h"

#include <memory>
#include <vector>

// Slots + training hits <-> XML. Used for the plugin state, exported profiles and the default
// profile. Message thread only.
//
//   <Profile version="1" window="20">
//     <Slot id="0" name="Kick" note="36"/>
//     <Hit slot="0" sr="48000" onset="240" peak="-12.5" scale="0.42">base64 int16 LE PCM</Hit>
//   </Profile>
//
// Only the audio of each training hit is stored (peak-normalised; sample = int16 / 32767 * scale);
// features are recomputed on load.
namespace ProfileIO
{
    inline constexpr const char* profileTag = "Profile";
    inline constexpr int kMaxTrainingHits = 2048;

    // Hits beyond kMaxTrainingHits, without a snippet, or with a label that is neither a slot
    // in `slots` nor kIgnoreSlotId are left out.
    std::unique_ptr<juce::XmlElement> toXml (const std::vector<bbr::SlotInfo>& slots,
                                             const std::vector<bbr::TrainingHit>& training,
                                             float windowMs);

    // Parses a <Profile> element. Every field is validated; invalid slots/hits are skipped
    // (hits whose slot isn't in the profile are dropped). Training features are recomputed for
    // windowOut. Returns false (outputs untouched) if `xml` is not a <Profile>.
    bool fromXml (const juce::XmlElement& xml,
                  std::vector<bbr::SlotInfo>& slotsOut,
                  std::vector<bbr::TrainingHit>& trainingOut,
                  float& windowOut);
}
