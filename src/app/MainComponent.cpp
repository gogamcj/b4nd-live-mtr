#include "app/MainComponent.h"
#include "ui/Theme.h"

namespace b4nd
{

MainComponent::MainComponent (AppState& s)
    : state (s), transportBar (s), sidePanel (s), timeline (s), mixer (s), statusBar (s), liveView (s)
{
    addAndMakeVisible (transportBar);
    addAndMakeVisible (sidePanel);
    addAndMakeVisible (timeline);
    addAndMakeVisible (mixer);
    addAndMakeVisible (statusBar);
    addChildComponent (liveView);

    transportBar.onLiveModeClicked = [this] { toggleLiveMode(); };
    liveView.onExitClicked = [this] { toggleLiveMode(); };

    setWantsKeyboardFocus (true);
    state.addChangeListener (this);
    setSize (1600, 960);
}

MainComponent::~MainComponent()
{
    state.removeChangeListener (this);
}

void MainComponent::toggleLiveMode()
{
    state.setLiveMode (! state.isLiveMode());
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (shownLive == state.isLiveMode())
        return;

    shownLive = state.isLiveMode();
    for (auto* c : std::initializer_list<juce::Component*> { &transportBar, &sidePanel, &timeline, &mixer, &statusBar })
        c->setVisible (! shownLive);
    liveView.setVisible (shownLive);
    resized();
    grabKeyboardFocus();

    if (onLiveModeChanged)
        onLiveModeChanged (shownLive);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    liveView.setBounds (r);

    transportBar.setBounds (r.removeFromTop (78));
    statusBar.setBounds (r.removeFromBottom (26));
    mixer.setBounds (r.removeFromBottom (juce::jlimit (230, 320, getHeight() / 3)));
    sidePanel.setBounds (r.removeFromLeft (juce::jlimit (220, 280, getWidth() / 6)));
    timeline.setBounds (r);
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // Default stage shortcuts (configurable later; footswitch mapping arrives in Phase 12).
    if (key == juce::KeyPress::spaceKey)             { state.togglePlayPause(); return true; }
    if (key == juce::KeyPress::escapeKey)            { state.emergencyStop();   return true; }
    if (key == juce::KeyPress::homeKey)              { state.restart();         return true; }
    if (key == juce::KeyPress::F11Key)               { toggleLiveMode();        return true; }
    if (key == juce::KeyPress::pageDownKey)          { state.nextSong();        return true; }
    if (key == juce::KeyPress::pageUpKey)            { state.previousSong();    return true; }
    if (key.getKeyCode() == 'L')                     { state.toggleLoop();      return true; }
    if (key.getKeyCode() == juce::KeyPress::rightKey){ state.jumpToNextCue();   return true; }
    if (key.getKeyCode() == juce::KeyPress::returnKey) { state.stop();          return true; }
    return false;
}

} // namespace b4nd
