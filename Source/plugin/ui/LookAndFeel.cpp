#include "LookAndFeel.h"

namespace ui
{

juce::Font font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (juce::jmax (4.0f, height), bold ? juce::Font::bold : juce::Font::plain));
}

juce::Colour slotColour (int slotId)
{
    if (slotId == bbr::kIgnoreSlotId)
        return Theme::ignored;

    if (slotId < 0 || slotId >= bbr::kMaxSlots)
        return Theme::unassigned;

    return BeatboxProcessor::getSlotColour (slotId);
}

juce::String noteNameOf (int note)
{
    return juce::String (bbr::noteName (juce::jlimit (0, 127, note)));
}

juce::String noteLabel (int note)
{
    return noteNameOf (note) + " (" + juce::String (note) + ")";
}

void fillSlotCombo (juce::ComboBox& box, const std::vector<BeatboxProcessor::Slot>& slots, bool includeChoose)
{
    const auto selected = box.getSelectedId();
    box.clear (juce::dontSendNotification);

    if (includeChoose)
        box.addItem ("- choose -", comboIdForSlot (bbr::kUnassigned));

    for (const auto& slot : slots)
        box.addItem (slot.name + "   " + noteNameOf (slot.note), comboIdForSlot (slot.id));

    box.addSeparator();
    box.addItem ("Ignore (no note)", comboIdForSlot (bbr::kIgnoreSlotId));

    if (selected != 0 && box.indexOfItemId (selected) >= 0)
        box.setSelectedId (selected, juce::dontSendNotification);
}

juce::String slotsSignature (const std::vector<BeatboxProcessor::Slot>& slots)
{
    juce::String sig;

    for (const auto& slot : slots)
        sig << slot.id << ':' << slot.note << ':' << slot.name << '\n';

    return sig;
}

void makeAccentButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, Theme::accent);
    b.setColour (juce::TextButton::textColourOffId, Theme::background);
}

void makeTabButton (juce::TextButton& b)
{
    b.getProperties().set ("bbrStyle", "tab");
}

//==================================================================================================
juce::AttributedString makeParagraph (const juce::String& text, float fontHeight, juce::Colour colour,
                                      juce::Justification justification)
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (justification);
    s.setLineSpacing (2.0f);

    juce::String run;
    bool bold = false;

    for (auto c : text)
    {
        if (c == '*')
        {
            if (run.isNotEmpty())
                s.append (run, font (fontHeight, bold), colour);

            run.clear();
            bold = ! bold;
            continue;
        }

        run += juce::String::charToString (c);
    }

    if (run.isNotEmpty())
        s.append (run, font (fontHeight, bold), colour);

    return s;
}

float paragraphHeight (const juce::String& text, float fontHeight, float width)
{
    if (text.isEmpty() || width <= 1.0f)
        return 0.0f;

    juce::TextLayout layout;
    layout.createLayout (makeParagraph (text, fontHeight, Theme::text), width);
    return std::ceil (layout.getHeight());
}

void drawParagraph (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                    float fontHeight, juce::Colour colour, juce::Justification justification)
{
    if (text.isEmpty() || area.isEmpty())
        return;

    juce::TextLayout layout;
    layout.createLayout (makeParagraph (text, fontHeight, colour, justification), area.getWidth());

    // Vertically centre when asked to.
    if (justification.testFlags (juce::Justification::verticallyCentred))
        area = area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), layout.getHeight()));

    layout.draw (g, area);
}

juce::Rectangle<int> paintPanel (juce::Graphics& g, juce::Rectangle<int> bounds,
                                 const juce::String& title, const juce::String& subtitle)
{
    auto r = bounds.toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (Theme::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    auto inner = bounds.reduced (kPanelPadding);

    if (title.isNotEmpty())
    {
        auto titleRow = inner.removeFromTop (kPanelTitleHeight - 4);
        g.setColour (Theme::text);
        g.setFont (font (16.0f, true));
        const auto titleWidth = juce::GlyphArrangement::getStringWidthInt (font (16.0f, true), title) + 12;
        g.drawText (title, titleRow.removeFromLeft (titleWidth), juce::Justification::centredLeft, false);

        if (subtitle.isNotEmpty())
        {
            g.setColour (Theme::textDim);
            g.setFont (font (13.0f));
            g.drawText (subtitle, titleRow, juce::Justification::centredLeft, true);
        }

        inner.removeFromTop (4);
    }

    return inner;
}

//==================================================================================================
static juce::Component& dialogHost (juce::Component& from)
{
    if (auto* editor = from.findParentComponentOfClass<juce::AudioProcessorEditor>())
        return *editor;

    if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (&from))
        return *editor;

    return *from.getTopLevelComponent();
}

void showMessage (juce::Component& from, const juce::String& title, const juce::String& message, bool isWarning)
{
    auto& host = dialogHost (from);
    auto options = juce::MessageBoxOptions()
                       .withIconType (isWarning ? juce::MessageBoxIconType::WarningIcon
                                                : juce::MessageBoxIconType::InfoIcon)
                       .withTitle (title)
                       .withMessage (message)
                       .withButton ("OK")
                       .withAssociatedComponent (&host)
                       .withParentComponent (&host);

    juce::AlertWindow::showAsync (options, [] (int) {});
}

void confirm (juce::Component& from, const juce::String& title, const juce::String& message,
              const juce::String& confirmText, std::function<void()> onConfirm)
{
    auto& host = dialogHost (from);
    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::QuestionIcon)
                       .withTitle (title)
                       .withMessage (message)
                       .withButton (confirmText)
                       .withButton ("Cancel")
                       .withAssociatedComponent (&host)
                       .withParentComponent (&host);

    juce::AlertWindow::showAsync (options, [onConfirm] (int result)
    {
        // With two buttons the first returns 1 and the second (Cancel / Escape) returns 0.
        if (result == 1 && onConfirm != nullptr)
            onConfirm();
    });
}

//==================================================================================================
static juce::LookAndFeel_V4::ColourScheme makeScheme()
{
    return { Theme::background,             // windowBackground
             Theme::panelRaised,            // widgetBackground
             Theme::panel,                  // menuBackground
             Theme::outline,                // outline
             Theme::text,                   // defaultText
             Theme::accent,                 // defaultFill
             Theme::text,                   // highlightedText
             Theme::accent.withAlpha (0.35f), // highlightedFill
             Theme::text };                 // menuText
}

BbrLookAndFeel::BbrLookAndFeel()
    : juce::LookAndFeel_V4 (makeScheme())
{
    setColour (juce::ResizableWindow::backgroundColourId, Theme::background);

    setColour (juce::TextButton::buttonColourId, Theme::panelRaised);
    setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    setColour (juce::TextButton::textColourOffId, Theme::text);
    setColour (juce::TextButton::textColourOnId, Theme::background);

    setColour (juce::ToggleButton::textColourId, Theme::text);
    setColour (juce::ToggleButton::tickColourId, Theme::accent);
    setColour (juce::ToggleButton::tickDisabledColourId, Theme::textFaint);

    setColour (juce::Label::textColourId, Theme::text);

    setColour (juce::ComboBox::backgroundColourId, Theme::panelRaised);
    setColour (juce::ComboBox::outlineColourId, Theme::outline);
    setColour (juce::ComboBox::textColourId, Theme::text);
    setColour (juce::ComboBox::arrowColourId, Theme::textDim);
    setColour (juce::ComboBox::focusedOutlineColourId, Theme::accent);

    setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
    setColour (juce::PopupMenu::textColourId, Theme::text);
    setColour (juce::PopupMenu::headerTextColourId, Theme::textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent.withAlpha (0.3f));
    setColour (juce::PopupMenu::highlightedTextColourId, Theme::text);

    setColour (juce::TextEditor::backgroundColourId, Theme::panelRaised);
    setColour (juce::TextEditor::textColourId, Theme::text);
    setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, Theme::text);
    setColour (juce::TextEditor::outlineColourId, Theme::outline);
    setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent);
    setColour (juce::CaretComponent::caretColourId, Theme::text);

    setColour (juce::Slider::backgroundColourId, Theme::outline);
    setColour (juce::Slider::trackColourId, Theme::accent);
    setColour (juce::Slider::thumbColourId, Theme::text);
    setColour (juce::Slider::textBoxTextColourId, Theme::text);
    setColour (juce::Slider::textBoxBackgroundColourId, Theme::panelRaised);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, Theme::accent.withAlpha (0.35f));

    setColour (juce::ProgressBar::backgroundColourId, Theme::outline);
    setColour (juce::ProgressBar::foregroundColourId, Theme::accent);

    setColour (juce::TooltipWindow::backgroundColourId, Theme::panelRaised.brighter (0.05f));
    setColour (juce::TooltipWindow::textColourId, Theme::text);
    setColour (juce::TooltipWindow::outlineColourId, Theme::outline.brighter (0.2f));

    setColour (juce::AlertWindow::backgroundColourId, Theme::panel);
    setColour (juce::AlertWindow::textColourId, Theme::text);
    setColour (juce::AlertWindow::outlineColourId, Theme::outline);

    setColour (juce::ScrollBar::thumbColourId, Theme::outline.brighter (0.3f));
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
}

juce::Font BbrLookAndFeel::getTextButtonFont (juce::TextButton& b, int buttonHeight)
{
    const bool big = buttonHeight >= 40;
    const bool tab = b.getProperties()["bbrStyle"].toString() == "tab";
    return font (big ? 16.0f : 14.0f, big || tab);
}

void BbrLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour,
                                           bool highlighted, bool down)
{
    auto bounds = b.getLocalBounds().toFloat().reduced (0.5f);

    if (b.getProperties()["bbrStyle"].toString() == "tab")
    {
        if (b.getToggleState())
        {
            g.setColour (Theme::panel);
            g.fillRoundedRectangle (bounds, 6.0f);
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (bounds.removeFromBottom (3.0f).reduced (10.0f, 0.0f), 1.5f);
        }
        else if (highlighted)
        {
            g.setColour (Theme::panel.withAlpha (0.6f));
            g.fillRoundedRectangle (bounds, 6.0f);
        }

        return;
    }

    auto colour = backgroundColour;

    if (! b.isEnabled())
        colour = colour.withMultipliedAlpha (0.45f);
    else if (down)
        colour = colour.darker (0.2f);
    else if (highlighted)
        colour = colour.brighter (0.12f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 6.0f);

    // Plain buttons get a hairline so they stand out from panels.
    if (backgroundColour == Theme::panelRaised)
    {
        g.setColour (Theme::outline.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    }

    if (b.hasKeyboardFocus (false) && b.isEnabled())
    {
        g.setColour (Theme::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (bounds.reduced (1.0f), 5.0f, 1.5f);
    }
}

void BbrLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    const bool tab = b.getProperties()["bbrStyle"].toString() == "tab";
    auto colour = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId
                                                   : juce::TextButton::textColourOffId);

    if (tab)
        colour = b.getToggleState() ? Theme::text : (highlighted ? Theme::text.withAlpha (0.85f) : Theme::textDim);

    if (! b.isEnabled())
        colour = colour.withMultipliedAlpha (0.45f);

    g.setColour (colour);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (10, 2),
                      juce::Justification::centred, 2, 0.85f);
}

void BbrLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto bounds = b.getLocalBounds().toFloat();
    const float h = juce::jmin (18.0f, bounds.getHeight() - 2.0f);
    const float w = h * 1.8f;
    auto track = juce::Rectangle<float> (bounds.getX() + 1.0f, bounds.getCentreY() - h * 0.5f, w, h);
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.45f;

    auto trackColour = on ? Theme::accent : Theme::outline.brighter (0.1f);

    if (highlighted && b.isEnabled())
        trackColour = trackColour.brighter (0.1f);

    g.setColour (trackColour.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, h * 0.5f);

    const float knob = h - 4.0f;
    const float knobX = on ? track.getRight() - knob - 2.0f : track.getX() + 2.0f;
    g.setColour (Theme::text.withMultipliedAlpha (alpha));
    g.fillEllipse (knobX, track.getY() + 2.0f, knob, knob);

    if (b.hasKeyboardFocus (false))
    {
        g.setColour (Theme::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (track.expanded (1.5f), h * 0.5f + 1.5f, 1.0f);
    }

    g.setColour (b.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (alpha));
    g.setFont (font (14.0f));
    g.drawFittedText (b.getButtonText(),
                      bounds.withTrimmedLeft (w + 10.0f).toNearestInt(),
                      juce::Justification::centredLeft, 2, 0.9f);
}

void BbrLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                   int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    const float alpha = box.isEnabled() ? 1.0f : 0.45f;

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (box.hasKeyboardFocus (true) ? Theme::accent : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto arrowZone = juce::Rectangle<float> ((float) width - 22.0f, 0.0f, 14.0f, (float) height);
    juce::Path arrow;
    arrow.startNewSubPath (arrowZone.getX() + 2.0f, arrowZone.getCentreY() - 2.5f);
    arrow.lineTo (arrowZone.getCentreX(), arrowZone.getCentreY() + 2.5f);
    arrow.lineTo (arrowZone.getRight() - 2.0f, arrowZone.getCentreY() - 2.5f);

    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (alpha));
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font BbrLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return font (juce::jmin (14.0f, (float) box.getHeight() * 0.55f));
}

juce::Font BbrLookAndFeel::getPopupMenuFont()
{
    return font (14.0f);
}

void BbrLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float minSliderPos, float maxSliderPos,
                                       juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float alpha = slider.isEnabled() ? 1.0f : 0.4f;
    const float cy = (float) y + (float) height * 0.5f;
    auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) width, 4.0f);

    g.setColour (slider.findColour (juce::Slider::backgroundColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, 2.0f);

    g.setColour (slider.findColour (juce::Slider::trackColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);

    const float r = slider.isMouseOverOrDragging() && slider.isEnabled() ? 8.0f : 7.0f;
    g.setColour (slider.findColour (juce::Slider::thumbColourId).withMultipliedAlpha (alpha));
    g.fillEllipse (sliderPos - r, cy - r, r * 2.0f, r * 2.0f);
    g.setColour (Theme::background.withAlpha (0.5f));
    g.drawEllipse (sliderPos - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);
}

juce::Label* BbrLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (font (13.0f));
    label->setJustificationType (juce::Justification::centred);
    label->setColour (juce::Label::backgroundColourId, Theme::panelRaised);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::textColourId, Theme::text);
    return label;
}

void BbrLookAndFeel::drawProgressBar (juce::Graphics& g, juce::ProgressBar& bar, int width, int height,
                                      double progress, const juce::String& textToShow)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat();
    const float radius = juce::jmin (bounds.getHeight() * 0.5f, 5.0f);

    g.setColour (bar.findColour (juce::ProgressBar::backgroundColourId));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (bar.findColour (juce::ProgressBar::foregroundColourId));

    if (progress >= 0.0 && progress <= 1.0)
    {
        g.fillRoundedRectangle (bounds.withWidth (juce::jmax (radius * 2.0f, bounds.getWidth() * (float) progress)), radius);
    }
    else
    {
        // Unknown progress: a block sliding back and forth.
        const auto t = (double) (juce::Time::getMillisecondCounter() % 1600u) / 1600.0;
        const auto phase = (float) (0.5 - 0.5 * std::cos (t * juce::MathConstants<double>::twoPi));
        const float blockW = bounds.getWidth() * 0.3f;
        g.fillRoundedRectangle (bounds.withWidth (blockW).withX (bounds.getX() + phase * (bounds.getWidth() - blockW)), radius);
    }

    if (textToShow.isNotEmpty() && height >= 14)
    {
        g.setColour (Theme::text);
        g.setFont (font (12.0f));
        g.drawText (textToShow, bounds, juce::Justification::centred, false);
    }
}

static juce::TextLayout layoutTooltip (const juce::String& text)
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.setWordWrap (juce::AttributedString::byWord);
    s.setLineSpacing (1.5f);
    s.append (text, font (13.0f), Theme::text);

    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 340.0f);
    return layout;
}

juce::Rectangle<int> BbrLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                       juce::Rectangle<int> parentArea)
{
    const auto layout = layoutTooltip (tipText);
    const auto w = (int) std::ceil (layout.getWidth() + 20.0f);
    const auto h = (int) std::ceil (layout.getHeight() + 14.0f);

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 20,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 12,
                                 w, h)
        .constrainedWithin (parentArea);
}

void BbrLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    auto bounds = juce::Rectangle<int> (width, height).toFloat();
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    layoutTooltip (text).draw (g, bounds.reduced (10.0f, 7.0f));
}

void BbrLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), 5.0f);
}

void BbrLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;

    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
    g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId
                                            : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), 5.0f, focused ? 1.5f : 1.0f);
}

juce::Font BbrLookAndFeel::getAlertWindowTitleFont()     { return font (17.0f, true); }
juce::Font BbrLookAndFeel::getAlertWindowMessageFont()   { return font (14.0f); }
juce::Font BbrLookAndFeel::getAlertWindowFont()          { return font (14.0f); }

//==================================================================================================
IconButton::IconButton (const juce::String& name, Icon iconToUse)
    : juce::Button (name), icon (iconToUse)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto bounds = getLocalBounds().toFloat();
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    auto square = bounds.withSizeKeepingCentre (size, size);

    if (isEnabled() && (highlighted || down))
    {
        g.setColour (Theme::text.withAlpha (down ? 0.18f : 0.1f));
        g.fillEllipse (square.reduced (0.5f));
    }

    auto icn = square.reduced (size * 0.28f);
    auto colour = isEnabled() ? iconColour : iconColour.withMultipliedAlpha (0.35f);
    g.setColour (colour);

    const float stroke = juce::jmax (1.4f, size * 0.08f);
    const juce::PathStrokeType strokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;

    switch (icon)
    {
        case Icon::play:
            p.addTriangle (icn.getX() + icn.getWidth() * 0.15f, icn.getY(),
                           icn.getX() + icn.getWidth() * 0.15f, icn.getBottom(),
                           icn.getRight(), icn.getCentreY());
            g.fillPath (p);
            break;

        case Icon::plus:
            p.startNewSubPath (icn.getCentreX(), icn.getY());
            p.lineTo (icn.getCentreX(), icn.getBottom());
            p.startNewSubPath (icn.getX(), icn.getCentreY());
            p.lineTo (icn.getRight(), icn.getCentreY());
            g.strokePath (p, strokeType);
            break;

        case Icon::minus:
            p.startNewSubPath (icn.getX(), icn.getCentreY());
            p.lineTo (icn.getRight(), icn.getCentreY());
            g.strokePath (p, strokeType);
            break;

        case Icon::trash:
        {
            const float w = icn.getWidth();
            const float h = icn.getHeight();
            p.startNewSubPath (icn.getX(), icn.getY() + h * 0.18f);
            p.lineTo (icn.getRight(), icn.getY() + h * 0.18f);
            p.startNewSubPath (icn.getX() + w * 0.36f, icn.getY() + h * 0.18f);
            p.lineTo (icn.getX() + w * 0.36f, icn.getY());
            p.lineTo (icn.getX() + w * 0.64f, icn.getY());
            p.lineTo (icn.getX() + w * 0.64f, icn.getY() + h * 0.18f);
            p.startNewSubPath (icn.getX() + w * 0.12f, icn.getY() + h * 0.3f);
            p.lineTo (icn.getX() + w * 0.2f, icn.getBottom());
            p.lineTo (icn.getX() + w * 0.8f, icn.getBottom());
            p.lineTo (icn.getX() + w * 0.88f, icn.getY() + h * 0.3f);
            g.strokePath (p, juce::PathStrokeType (stroke * 0.85f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
            break;
        }

        case Icon::cross:
            p.startNewSubPath (icn.getTopLeft());
            p.lineTo (icn.getBottomRight());
            p.startNewSubPath (icn.getTopRight());
            p.lineTo (icn.getBottomLeft());
            g.strokePath (p, strokeType);
            break;
    }
}

//==================================================================================================
StepList::StepList (bool shouldNumber, float height)
    : numbered (shouldNumber), fontHeight (height)
{
    setInterceptsMouseClicks (false, false);
}

void StepList::setItems (const juce::StringArray& newItems)
{
    if (newItems == items)
        return;

    items = newItems;
    repaint();
}

int StepList::getHeightForWidth (int width) const
{
    const auto textWidth = (float) (width - kGutter);
    float total = 0.0f;

    for (const auto& item : items)
        total += juce::jmax (fontHeight + 6.0f, paragraphHeight (item, fontHeight, textWidth)) + (float) kSpacing;

    return (int) std::ceil (juce::jmax (0.0f, total - (float) kSpacing));
}

void StepList::paint (juce::Graphics& g)
{
    const auto textWidth = (float) (getWidth() - kGutter);
    float y = 0.0f;
    int number = 1;

    for (const auto& item : items)
    {
        const auto h = juce::jmax (fontHeight + 6.0f, paragraphHeight (item, fontHeight, textWidth));

        if (numbered)
        {
            const float d = fontHeight + 6.0f;
            auto circle = juce::Rectangle<float> (0.0f, y - 2.0f, d, d);
            g.setColour (Theme::accent.withAlpha (0.18f));
            g.fillEllipse (circle);
            g.setColour (Theme::accent);
            g.setFont (font (fontHeight - 1.0f, true));
            g.drawText (juce::String (number), circle, juce::Justification::centred, false);
        }
        else
        {
            g.setColour (Theme::accent);
            g.fillEllipse (6.0f, y + fontHeight * 0.5f - 2.0f, 5.0f, 5.0f);
        }

        drawParagraph (g, item, { (float) kGutter, y, textWidth, h }, fontHeight, Theme::text);
        y += h + (float) kSpacing;
        ++number;
    }
}

} // namespace ui
