#include "ui/ShowCheckPanel.h"
#include "ui/AudioSettingsPanel.h"
#include "ui/Theme.h"

namespace b4nd
{

ShowCheckPanel::ShowCheckPanel (AppState& s, std::function<void()> live) : state (s), enterLive (std::move (live))
{
    recheckButton.onClick = [this] { recheck(); };
    audioButton.onClick = [this] { showAudioSettingsWindow (state); };
    liveButton.onClick = [this]
    {
        if (! enterLive)
            return;
        if (! AppState::hasBlockers (items))
        {
            enterLive();
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setVisible (false);
            return;
        }
        juce::Component::SafePointer<ShowCheckPanel> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Enter Live Mode anyway?",
                                            "Some checks are red. Entering Live Mode now is for a rehearsal, not for a show.",
                                            "Enter anyway", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create ([safe] (int ok) {
                                                if (ok != 0 && safe != nullptr && safe->enterLive)
                                                {
                                                    juce::Logger::writeToLog ("LIVE MODE entered with red show checks (user override)");
                                                    safe->enterLive();
                                                    if (auto* w = safe->findParentComponentOfClass<juce::DocumentWindow>())
                                                        w->setVisible (false);
                                                }
                                            }));
    };
    for (auto* b : { &recheckButton, &audioButton, &liveButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    setSize (760, 600);
    recheck();
    startTimer (2000);
}

void ShowCheckPanel::recheck()
{
    items = state.runShowCheck();
    const bool blocked = AppState::hasBlockers (items);
    liveButton.setVisible ((bool) enterLive && ! state.isLiveMode());
    liveButton.setButtonText (blocked ? "ENTER ANYWAY (REHEARSAL)" : "ENTER LIVE MODE");
    liveButton.setColour (juce::TextButton::buttonColourId, blocked ? theme::danger.darker (0.4f) : theme::play.darker (0.3f));
    repaint();
}

void ShowCheckPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    using L = AppState::ShowCheckItem::Level;
    int blocks = 0, warns = 0;
    for (const auto& i : items)
    {
        blocks += i.level == L::Block ? 1 : 0;
        warns += i.level == L::Warn ? 1 : 0;
    }
    auto r = getLocalBounds().reduced (16);
    auto head = r.removeFromTop (34);
    g.setFont (theme::font (19.0f, true));
    g.setColour (blocks > 0 ? theme::danger : warns > 0 ? theme::warn : theme::ok);
    g.drawText (blocks > 0 ? "NOT READY FOR THE SHOW: " + juce::String (blocks) + " red"
                : warns > 0 ? "READY, " + juce::String (warns) + " warning" + (warns > 1 ? "s" : "")
                            : juce::String ("READY FOR THE SHOW"),
                head, juce::Justification::centredLeft);

    auto list = listArea;
    for (const auto& i : items)
    {
        auto row = list.removeFromTop (34);
        if (row.getBottom() > listArea.getBottom())
            break;
        const auto c = i.level == L::Block ? theme::danger : i.level == L::Warn ? theme::warn : i.level == L::Info ? theme::accent : theme::ok;
        g.setColour (c.withAlpha (0.12f));
        g.fillRoundedRectangle (row.reduced (0, 2).toFloat(), 4.0f);
        auto badge = row.removeFromLeft (64).reduced (6, 7);
        g.setColour (c);
        g.fillRoundedRectangle (badge.toFloat(), 3.0f);
        g.setColour (juce::Colours::black);
        g.setFont (theme::font (11.0f, true));
        g.drawText (i.level == L::Block ? "STOP" : i.level == L::Warn ? "CHECK" : i.level == L::Info ? "INFO" : "OK", badge,
                    juce::Justification::centred);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (i.title, row.removeFromLeft (170).withTrimmedLeft (8), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (i.detail, row.withTrimmedRight (6), juce::Justification::centredLeft, 2, 0.8f);
    }
}

void ShowCheckPanel::resized()
{
    auto r = getLocalBounds().reduced (16);
    r.removeFromTop (40);
    auto buttons = r.removeFromBottom (40);
    recheckButton.setBounds (buttons.removeFromLeft (130).reduced (0, 4));
    buttons.removeFromLeft (8);
    audioButton.setBounds (buttons.removeFromLeft (190).reduced (0, 4));
    liveButton.setBounds (buttons.removeFromRight (220).reduced (0, 4));
    r.removeFromBottom (8);
    listArea = r;
}

//==============================================================================
namespace
{
class ShowCheckWindow : public juce::DocumentWindow
{
public:
    ShowCheckWindow (AppState& s, std::function<void()> enterLive)
        : DocumentWindow ("Show Check - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new ShowCheckPanel (s, std::move (enterLive)), true);
        setResizable (true, false);
        setResizeLimits (640, 480, 1400, 1200);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<ShowCheckWindow> showCheckOwner;
} // namespace

void showShowCheckWindow (AppState& state, std::function<void()> enterLive)
{
    showCheckOwner = std::make_unique<ShowCheckWindow> (state, std::move (enterLive));   // fresh check each time
    showCheckOwner->toFront (true);
}

ShowCheckPanel* getShowCheckWindowContent()
{
    return showCheckOwner != nullptr ? dynamic_cast<ShowCheckPanel*> (showCheckOwner->getContentComponent()) : nullptr;
}

void closeShowCheckWindow()
{
    showCheckOwner.reset();
}

} // namespace b4nd
