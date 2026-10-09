#include "SlotsPanel.h"

#include <algorithm>

namespace ui
{

namespace
{
    constexpr int kRowHeight = 38;
    constexpr int kNoteWidth = 92;
    constexpr int kCountWidth = 34;
    constexpr int kDeleteWidth = 26;
}

//==================================================================================================
class SlotsPanel::Row : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    Row (BeatboxProcessor& p, int id)
        : processor (p), slotId (id)
    {
        nameEditor.setFont (font (14.0f));
        nameEditor.setIndents (8, 0);
        nameEditor.setJustification (juce::Justification::centredLeft);
        nameEditor.setInputRestrictions (24);
        nameEditor.setSelectAllWhenFocused (true);
        nameEditor.setTooltip ("Name of this sound - click to rename");
        nameEditor.onReturnKey = [this] { commitName(); nameEditor.giveAwayKeyboardFocus(); };
        nameEditor.onEscapeKey = [this] { nameEditor.setText (currentName, false); nameEditor.giveAwayKeyboardFocus(); };
        nameEditor.onFocusLost = [this] { commitName(); };
        addAndMakeVisible (nameEditor);

        for (int n = 0; n < 128; ++n)
            noteBox.addItem (noteLabel (n), n + 1);

        noteBox.setTooltip ("The MIDI note this sound plays. Match it to the pad in your Drum Rack "
                            "(Kick C1, Snare D1, Closed hat F#1, Open hat A#1).");
        noteBox.onChange = [this]
        {
            const auto itemId = noteBox.getSelectedId();

            if (itemId > 0 && processor.hasSlot (slotId))
                processor.setSlotNote (slotId, itemId - 1);
        };
        addAndMakeVisible (noteBox);

        deleteButton.setTooltip ("Delete this sound and its training examples");
        deleteButton.setIconColour (Theme::textDim);
        deleteButton.onClick = [this] { askDelete(); };
        addAndMakeVisible (deleteButton);
    }

    ~Row() override
    {
        nameEditor.onFocusLost = nullptr;
        nameEditor.onReturnKey = nullptr;
        nameEditor.onEscapeKey = nullptr;
    }

    int getSlotId() const noexcept   { return slotId; }

    void update (const BeatboxProcessor::Slot& slot, int trainingCount)
    {
        colour = slotColour (slot.id);
        currentName = slot.name;

        if (! nameEditor.hasKeyboardFocus (true) && nameEditor.getText() != slot.name)
            nameEditor.setText (slot.name, false);

        if (noteBox.getSelectedId() != slot.note + 1)
            noteBox.setSelectedId (slot.note + 1, juce::dontSendNotification);

        if (trainingCount != examples)
        {
            examples = trainingCount;
            setTooltip (examples == 0 ? "No training examples yet - teach this sound on the Learn tab. Right-click for more."
                                      : juce::String (examples) + " training examples. Right-click to play one or clear them.");
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (0, 5);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (6).toFloat(), 3.0f);

        auto countArea = getLocalBounds().withTrimmedRight (kDeleteWidth + 2).removeFromRight (kCountWidth);
        g.setFont (font (14.0f, examples > 0));
        g.setColour (examples > 0 ? Theme::text : Theme::warning);
        g.drawText (juce::String (examples), countArea, juce::Justification::centredRight, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (0, 5);
        r.removeFromLeft (14);
        deleteButton.setBounds (r.removeFromRight (kDeleteWidth).withSizeKeepingCentre (kDeleteWidth, kDeleteWidth));
        r.removeFromRight (2 + kCountWidth + 8);
        noteBox.setBounds (r.removeFromRight (kNoteWidth));
        r.removeFromRight (6);
        nameEditor.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            return;

        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addSectionHeader (currentName);
        menu.addItem (1, "Play a training example", examples > 0);
        menu.addItem (2, "Clear its " + juce::String (examples) + " examples", examples > 0);
        menu.addSeparator();
        menu.addItem (3, "Delete sound");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safeThis = juce::Component::SafePointer<Row> (this)] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)
                safeThis->playExample();
            else if (result == 2)
                safeThis->askClear();
            else if (result == 3)
                safeThis->askDelete();
        });
    }

private:
    BeatboxProcessor& processor;
    const int slotId;
    juce::TextEditor nameEditor;
    juce::ComboBox noteBox;
    IconButton deleteButton { "Delete", IconButton::Icon::trash };
    juce::String currentName;
    juce::Colour colour { Theme::unassigned };
    int examples = -1;
    int auditionCursor = 0;

    void commitName()
    {
        const auto text = nameEditor.getText().trim();

        if (text.isEmpty())
        {
            nameEditor.setText (currentName, false);
            return;
        }

        if (text != currentName && processor.hasSlot (slotId))
        {
            currentName = text;
            processor.setSlotName (slotId, text);
        }
    }

    void playExample()
    {
        const auto& hits = processor.getTrainingHits();
        std::vector<int> indices;

        for (size_t i = 0; i < hits.size(); ++i)
            if (hits[i].slotId == slotId)
                indices.push_back ((int) i);

        if (indices.empty())
            return;

        const auto pick = indices[(size_t) (auditionCursor++ % (int) indices.size())];
        processor.auditionTrainingHit (pick);
    }

    void askClear()
    {
        confirm (*this, "Clear examples for \"" + currentName + "\"?",
                 "This deletes its " + juce::String (examples) + " training examples. The sound itself stays, "
                 "so you can teach it again with Learn.",
                 "Clear",
                 [safeThis = juce::Component::SafePointer<Row> (this)]
                 {
                     if (safeThis != nullptr && safeThis->processor.hasSlot (safeThis->slotId))
                         safeThis->processor.clearTrainingFor (safeThis->slotId);
                 });
    }

    void askDelete()
    {
        const auto count = processor.getTrainingCount (slotId);
        confirm (*this, "Delete \"" + currentName + "\"?",
                 count > 0 ? "Its " + juce::String (count) + " training examples will be deleted too."
                           : juce::String ("It has no training examples yet."),
                 "Delete",
                 [safeThis = juce::Component::SafePointer<Row> (this)]
                 {
                     if (safeThis != nullptr && safeThis->processor.hasSlot (safeThis->slotId))
                         safeThis->processor.removeSlot (safeThis->slotId);
                 });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
};

//==================================================================================================
SlotsPanel::SlotsPanel (BeatboxProcessor& p)
    : processor (p)
{
    viewport.setViewedComponent (&rowsContent, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);

    addButton.setTooltip ("Add another sound (up to 8), then teach it with Learn.");
    addButton.onClick = [this]
    {
        const auto id = processor.addSlot();
        refresh();

        for (auto& row : rows)
            if (row != nullptr && row->getSlotId() == id)
                viewport.setViewPosition (0, row->getY());
    };
    addAndMakeVisible (addButton);

    clearButton.setTooltip ("Delete every training example. Your sounds and their notes stay.");
    // The SafePointer is made in the outer lambda: MSVC resolves `this` inside a nested lambda's
    // init-capture to the outer closure, not to the panel.
    clearButton.onClick = [this, safeThis = juce::Component::SafePointer<SlotsPanel> (this)]
    {
        confirm (*this, "Clear all training?",
                 "This deletes all " + juce::String (totalCount) + " training examples, including Ignore. "
                 "Your sounds and their notes stay.",
                 "Clear all",
                 [safeThis]
                 {
                     if (safeThis != nullptr)
                         safeThis->processor.clearTraining();
                 });
    };
    addAndMakeVisible (clearButton);

    refresh();
}

SlotsPanel::~SlotsPanel() = default;

void SlotsPanel::refresh()
{
    const auto slots = processor.getSlots();
    std::vector<int> ids;

    for (const auto& slot : slots)
        ids.push_back (slot.id);

    if (ids != shownIds)
    {
        // Keep rows whose slot still exists (so a name being edited isn't lost).
        std::vector<std::unique_ptr<Row>> newRows;

        for (const auto& slot : slots)
        {
            auto it = std::find_if (rows.begin(), rows.end(),
                                    [&slot] (const std::unique_ptr<Row>& r) { return r != nullptr && r->getSlotId() == slot.id; });

            if (it != rows.end())
            {
                newRows.push_back (std::move (*it));
            }
            else
            {
                auto row = std::make_unique<Row> (processor, slot.id);
                rowsContent.addAndMakeVisible (*row);
                row->sendLookAndFeelChange();
                newRows.push_back (std::move (row));
            }
        }

        rows = std::move (newRows);
        shownIds = ids;
        layoutRows();
    }

    for (size_t i = 0; i < rows.size() && i < slots.size(); ++i)
        rows[i]->update (slots[i], processor.getTrainingCount (slots[i].id));

    addButton.setEnabled ((int) slots.size() < bbr::kMaxSlots);
    addButton.setTooltip ((int) slots.size() < bbr::kMaxSlots ? "Add another sound (up to 8), then teach it with Learn."
                                                              : "You have the maximum of 8 sounds.");

    ignoreCount = processor.getTrainingCount (bbr::kIgnoreSlotId);
    totalCount = (int) processor.getTrainingHits().size();
    clearButton.setEnabled (totalCount > 0);

    repaint();
}

void SlotsPanel::paint (juce::Graphics& g)
{
    paintPanel (g, getLocalBounds(), "Your sounds",
                totalCount > 0 ? juce::String (totalCount) + " examples" : juce::String());

    g.setFont (font (12.0f));
    g.setColour (Theme::textFaint);
    const auto header = columnHeaderArea;
    const auto right = header.getRight() - (rowsContent.getWidth() < viewport.getWidth() ? viewport.getScrollBarThickness() + 2 : 0);
    g.drawText ("Sound", header.withTrimmedLeft (14), juce::Justification::centredLeft, false);
    g.drawText ("Note", header.withX (right - kDeleteWidth - (2 + kCountWidth + 8) - kNoteWidth).withWidth (kNoteWidth),
                juce::Justification::centredLeft, false);
    g.drawText ("Examples", header.withX (right - kDeleteWidth - 2 - 70).withWidth (70),
                juce::Justification::centredRight, false);

    if (rows.empty())
    {
        drawParagraph (g, "No sounds yet. Click *+ Add sound* to make one.", viewport.getBounds().toFloat().reduced (4.0f),
                       14.0f, Theme::textDim, juce::Justification::centredTop);
    }

    auto footer = footerTextArea;
    g.setFont (font (13.0f));
    g.setColour (Theme::textDim);
    g.drawText ("Ignore (no note): " + juce::String (ignoreCount) + (ignoreCount == 1 ? " example" : " examples"),
                footer, juce::Justification::centredLeft, true);
}

void SlotsPanel::resized()
{
    auto inner = getLocalBounds().reduced (kPanelPadding);
    inner.removeFromTop (kPanelTitleHeight);
    columnHeaderArea = inner.removeFromTop (18);

    auto footer = inner.removeFromBottom (58);
    footerTextArea = footer.removeFromTop (22);
    footer.removeFromTop (4);
    clearButton.setBounds (footer.removeFromTop (30).removeFromLeft (juce::jmin (160, footer.getWidth())));

    inner.removeFromBottom (8);
    addButton.setBounds (inner.removeFromBottom (30).removeFromLeft (juce::jmin (130, inner.getWidth())));
    inner.removeFromBottom (8);
    viewport.setBounds (inner);
    layoutRows();
}

void SlotsPanel::layoutRows()
{
    const auto width = viewport.getMaximumVisibleWidth();
    const auto needed = (int) rows.size() * kRowHeight;
    const auto w = needed > viewport.getHeight() ? width - viewport.getScrollBarThickness() - 2 : width;

    rowsContent.setSize (juce::jmax (0, w), needed);

    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, (int) i * kRowHeight, rowsContent.getWidth(), kRowHeight);
}

} // namespace ui
