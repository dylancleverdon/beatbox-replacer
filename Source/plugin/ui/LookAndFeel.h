#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"

#include <functional>
#include <vector>

// Colours, fonts, the plugin's LookAndFeel and a few small widgets shared by the UI.
namespace ui
{

namespace Theme
{
    inline const juce::Colour background  { 0xff16181d };
    inline const juce::Colour panel       { 0xff1f232b };
    inline const juce::Colour panelRaised { 0xff292e38 };
    inline const juce::Colour outline     { 0xff363c48 };
    inline const juce::Colour accent      { 0xffff7a45 };
    inline const juce::Colour text        { 0xffe8eaed };
    inline const juce::Colour textDim     { 0xffa0a6b0 };
    inline const juce::Colour textFaint   { 0xff6c727d };
    inline const juce::Colour good        { 0xff4cc38a };
    inline const juce::Colour warning     { 0xffffc04d };
    inline const juce::Colour danger      { 0xffff5c5c };
    inline const juce::Colour recording   { 0xffe5484d };
    inline const juce::Colour unassigned  { 0xff8a909a };
    inline const juce::Colour ignored     { 0xff4a505b };
}

juce::Font font (float height, bool bold = false);

// Slot colour from the processor's palette; grey for unassigned, dark grey for ignore.
juce::Colour slotColour (int slotId);

// "C1 (36)" using Ableton's naming (60 = C3).
juce::String noteLabel (int note);
juce::String noteNameOf (int note);

// ComboBox item ids for slot choices (item ids must be non-zero):
// kUnassigned -> 1, kIgnoreSlotId -> 2, slot n -> n + 3.
constexpr int comboIdForSlot (int slotId) noexcept   { return slotId + 3; }
constexpr int slotForComboId (int itemId) noexcept   { return itemId - 3; }

// Fills a combo with "- choose -" (optional), every slot ("Kick  C1") and "Ignore (no note)".
void fillSlotCombo (juce::ComboBox&, const std::vector<BeatboxProcessor::Slot>&, bool includeChoose);

// A string that changes whenever a slot is added, removed, renamed or re-noted.
juce::String slotsSignature (const std::vector<BeatboxProcessor::Slot>&);

// Button styles drawn by BbrLookAndFeel: plain (default), accent, tab.
void makeAccentButton (juce::TextButton&);
void makeTabButton (juce::TextButton&);

// Wrapped paragraph; text between '*' characters is drawn bold.
juce::AttributedString makeParagraph (const juce::String& text, float fontHeight, juce::Colour colour,
                                      juce::Justification = juce::Justification::topLeft);
float paragraphHeight (const juce::String& text, float fontHeight, float width);
void drawParagraph (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area,
                    float fontHeight, juce::Colour colour,
                    juce::Justification = juce::Justification::topLeft);

// Rounded panel background with an optional title (and subtitle) at the top. Returns the space
// below the title.
juce::Rectangle<int> paintPanel (juce::Graphics&, juce::Rectangle<int> bounds,
                                 const juce::String& title = {}, const juce::String& subtitle = {});
constexpr int kPanelPadding = 14;
constexpr int kPanelTitleHeight = 26;

// Message boxes shown inside the plugin window (no modal loops; safe in AU and VST3 hosts).
void showMessage (juce::Component& from, const juce::String& title, const juce::String& message,
                  bool isWarning = false);
// OK / Cancel; onConfirm runs only if the user confirms. Capture SafePointers in onConfirm.
void confirm (juce::Component& from, const juce::String& title, const juce::String& message,
              const juce::String& confirmText, std::function<void()> onConfirm);

//==================================================================================================
class BbrLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BbrLookAndFeel();

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int width, int height,
                          double progress, const juce::String& textToShow) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BbrLookAndFeel)
};

//==================================================================================================
// Small round button that draws a vector icon.
class IconButton : public juce::Button
{
public:
    enum class Icon
    {
        play,
        plus,
        minus,
        trash,
        cross
    };

    IconButton (const juce::String& name, Icon);

    void setIconColour (juce::Colour c)    { iconColour = c; repaint(); }
    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    Icon icon;
    juce::Colour iconColour { Theme::text };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};

//==================================================================================================
// A numbered (or bulleted) list of wrapped paragraphs ('*bold*' supported).
class StepList : public juce::Component
{
public:
    explicit StepList (bool numbered = true, float fontHeight = 14.0f);

    void setItems (const juce::StringArray&);
    int getHeightForWidth (int width) const;

    void paint (juce::Graphics&) override;

private:
    juce::StringArray items;
    bool numbered;
    float fontHeight;

    static constexpr int kGutter = 28;
    static constexpr int kSpacing = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepList)
};

} // namespace ui
