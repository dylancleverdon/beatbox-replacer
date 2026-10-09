#include "ProfileIO.h"

#include "../core/Analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>

namespace ProfileIO
{

namespace
{
    constexpr int kFormatVersion = 1;
    constexpr double kMinSampleRate = 8000.0;
    constexpr double kMaxSampleRate = 768000.0;
    constexpr double kMaxSnippetSeconds = 2.0;
    constexpr int kMaxNameLength = 64;

    bool isValidLabel (int slotId, const std::set<int>& slotIds)
    {
        return slotId == bbr::kIgnoreSlotId || slotIds.count (slotId) > 0;
    }

    // Strict attribute readers: a missing or non-numeric attribute is an error rather than 0.
    bool readInt (const juce::XmlElement& e, const char* name, int& out)
    {
        if (! e.hasAttribute (name))
            return false;

        const auto text = e.getStringAttribute (name).trim();

        if (text.isEmpty() || text.length() > 11 || ! text.containsOnly ("+-0123456789"))
            return false;

        const auto value = text.getLargeIntValue();

        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            return false;

        out = (int) value;
        return true;
    }

    bool readDouble (const juce::XmlElement& e, const char* name, double& out)
    {
        if (! e.hasAttribute (name))
            return false;

        const auto text = e.getStringAttribute (name).trim();

        if (text.isEmpty() || text.length() > 40 || ! text.containsOnly ("+-.0123456789eE"))
            return false;

        const double value = text.getDoubleValue();

        if (! std::isfinite (value))
            return false;

        out = value;
        return true;
    }

    // Each snippet is stored peak-normalised (its scale in the "scale" attribute), so quiet
    // sounds keep the full 16-bit resolution and loud ones don't clip.
    juce::String encodeSnippet (const std::vector<float>& snippet, float& scaleOut)
    {
        float peak = 0.0f;

        for (const float x : snippet)
            if (std::isfinite (x))
                peak = std::max (peak, std::abs (x));

        scaleOut = peak > 1.0e-9f ? peak : 1.0f;
        const float gain = 32767.0f / scaleOut;

        juce::MemoryBlock pcm (snippet.size() * 2, false);
        auto* bytes = static_cast<uint8_t*> (pcm.getData());

        for (size_t i = 0; i < snippet.size(); ++i)
        {
            const float x = std::isfinite (snippet[i]) ? snippet[i] : 0.0f;
            const auto v = (int16_t) juce::jlimit (-32767, 32767, juce::roundToInt (x * gain));
            const auto u = (uint16_t) v;
            bytes[2 * i] = (uint8_t) (u & 0xffu);
            bytes[2 * i + 1] = (uint8_t) (u >> 8);
        }

        return juce::Base64::toBase64 (pcm.getData(), pcm.getSize());
    }

    bool decodeSnippet (const juce::String& base64, float scale, size_t maxSamples, std::vector<float>& out)
    {
        const auto text = base64.removeCharacters (" \t\r\n");

        if (text.isEmpty() || text.length() % 4 != 0
            || (size_t) text.length() / 4 * 3 > maxSamples * 2 + 2)
            return false;

        juce::MemoryOutputStream pcm;

        if (! juce::Base64::convertFromBase64 (pcm, text))
            return false;

        const size_t numBytes = pcm.getDataSize();

        if (numBytes < 2 || numBytes % 2 != 0 || numBytes / 2 > maxSamples)
            return false;

        const auto* bytes = static_cast<const uint8_t*> (pcm.getData());
        const float gain = scale / 32767.0f;
        out.resize (numBytes / 2);

        for (size_t i = 0; i < out.size(); ++i)
        {
            const auto u = (uint16_t) (bytes[2 * i] | (bytes[2 * i + 1] << 8));
            out[i] = (float) (int16_t) u * gain;
        }

        return true;
    }
}

std::unique_ptr<juce::XmlElement> toXml (const std::vector<bbr::SlotInfo>& slots,
                                         const std::vector<bbr::TrainingHit>& training,
                                         float windowMs)
{
    auto xml = std::make_unique<juce::XmlElement> (profileTag);
    xml->setAttribute ("version", kFormatVersion);
    xml->setAttribute ("window", (double) windowMs);

    std::set<int> slotIds;

    for (const auto& slot : slots)
    {
        if (slot.id < 0 || slot.id >= bbr::kMaxSlots || slotIds.count (slot.id) > 0)
            continue;

        slotIds.insert (slot.id);
        auto* e = xml->createNewChildElement ("Slot");
        e->setAttribute ("id", slot.id);
        e->setAttribute ("name", juce::String::fromUTF8 (slot.name.c_str()));
        e->setAttribute ("note", juce::jlimit (0, 127, slot.note));
    }

    int written = 0;

    for (const auto& hit : training)
    {
        if (written >= kMaxTrainingHits)
            break;

        if (! isValidLabel (hit.slotId, slotIds) || hit.snippet.empty()
            || ! (hit.sampleRate >= kMinSampleRate && hit.sampleRate <= kMaxSampleRate))
            continue;

        float scale = 1.0f;
        const auto pcm = encodeSnippet (hit.snippet, scale);

        auto* e = xml->createNewChildElement ("Hit");
        e->setAttribute ("slot", hit.slotId);
        e->setAttribute ("sr", hit.sampleRate);
        e->setAttribute ("onset", juce::jlimit (0, (int) hit.snippet.size() - 1, hit.onsetOffset));
        e->setAttribute ("peak", std::isfinite (hit.peakDb) ? (double) hit.peakDb : -120.0);

        e->setAttribute ("scale", (double) scale);

        e->addTextElement (pcm);
        ++written;
    }

    return xml;
}

bool fromXml (const juce::XmlElement& xml,
              std::vector<bbr::SlotInfo>& slotsOut,
              std::vector<bbr::TrainingHit>& trainingOut,
              float& windowOut)
{
    if (! xml.hasTagName (profileTag))
        return false;

    double window = 20.0;

    if (! readDouble (xml, "window", window))
        window = 20.0;

    const float windowMs = (float) juce::jlimit ((double) bbr::kMinWindowMs, (double) bbr::kMaxWindowMs, window);

    std::vector<bbr::SlotInfo> slots;
    std::set<int> slotIds;

    for (auto* e : xml.getChildWithTagNameIterator ("Slot"))
    {
        int id = -1, note = -1;

        if (! readInt (*e, "id", id) || ! readInt (*e, "note", note))
            continue;

        if (id < 0 || id >= bbr::kMaxSlots || slotIds.count (id) > 0 || note < 0 || note > 127)
            continue;

        auto name = e->getStringAttribute ("name").trim().substring (0, kMaxNameLength);

        if (name.isEmpty())
            name = "Sound " + juce::String (id + 1);

        bbr::SlotInfo slot;
        slot.id = id;
        slot.name = name.toStdString();
        slot.note = note;
        slots.push_back (std::move (slot));
        slotIds.insert (id);
    }

    std::sort (slots.begin(), slots.end(), [] (const auto& a, const auto& b) { return a.id < b.id; });

    std::vector<bbr::TrainingHit> training;

    for (auto* e : xml.getChildWithTagNameIterator ("Hit"))
    {
        if ((int) training.size() >= kMaxTrainingHits)
            break;

        int slotId = bbr::kUnassigned, onset = -1;
        double sampleRate = 0.0, peak = -120.0, scale = 1.0;

        if (! readInt (*e, "slot", slotId) || ! isValidLabel (slotId, slotIds))
            continue;

        if (! readDouble (*e, "sr", sampleRate) || sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate)
            continue;

        if (! readInt (*e, "onset", onset) || onset < 0)
            continue;

        if (! readDouble (*e, "peak", peak))
            peak = -120.0;

        if (e->hasAttribute ("scale") && (! readDouble (*e, "scale", scale) || scale <= 0.0 || scale > 1.0e6))
            continue;

        bbr::TrainingHit hit;
        const auto maxSamples = (size_t) std::ceil (sampleRate * kMaxSnippetSeconds);

        if (! decodeSnippet (e->getAllSubText(), (float) scale, maxSamples, hit.snippet))
            continue;

        if ((size_t) onset >= hit.snippet.size())
            continue;

        hit.slotId = slotId;
        hit.sampleRate = sampleRate;
        hit.onsetOffset = onset;
        hit.peakDb = (float) juce::jlimit (-200.0, 200.0, peak);
        training.push_back (std::move (hit));
    }

    bbr::FeatureSettings settings;
    settings.windowMs = windowMs;
    bbr::recomputeFeatures (training, settings);

    slotsOut = std::move (slots);
    trainingOut = std::move (training);
    windowOut = windowMs;
    return true;
}

} // namespace ProfileIO
