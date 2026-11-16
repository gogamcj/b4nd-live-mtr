#pragma once

#include "app/AppState.h"
#include "ui/LiveModeView.h"
#include "ui/MixerPanel.h"
#include "ui/SidePanel.h"
#include "ui/StatusBar.h"
#include "ui/TimelineView.h"
#include "ui/TransportBar.h"

namespace b4nd
{

/** Root content: edit layout (top / left / centre / bottom) or the full-screen live view. */
class MainComponent : public juce::Component,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (AppState&);
    ~MainComponent() override;

    /** Invoked when the live mode toggles so the window can go full-screen. */
    std::function<void (bool)> onLiveModeChanged;

    void toggleLiveMode();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    AppState& state;
    TransportBar transportBar;
    SidePanel sidePanel;
    TimelineView timeline;
    MixerPanel mixer;
    StatusBar statusBar;
    LiveModeView liveView;
    juce::TooltipWindow tooltips { this, 600 };
    bool shownLive = false;
};

} // namespace b4nd
