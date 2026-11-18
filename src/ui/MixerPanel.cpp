#include "ui/MixerPanel.h"
#include "ui/Theme.h"

namespace b4nd
{

static constexpr int stripWidth = 78;
static constexpr int outputMeterWidthNormal = 46;
static constexpr int outputMeterWidthWide = 96;   // 2-3 output devices: room for "MAIN MONO" / "IN-EAR"

//==============================================================================
ChannelStrip::ChannelStrip (AppState& s, int trackIndex) : state (s), track (trackIndex)
{
    fader.setRange (-60.0, 6.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTextValueSuffix (" dB");
    fader.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 60, 16);
    fader.setNumDecimalPlacesToDisplay (1);
    fader.setWantsKeyboardFocus (false);
    addAndMakeVisible (fader);

    pan.setRange (-1.0, 1.0, 0.01);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setWantsKeyboardFocus (false);
    addAndMakeVisible (pan);

    for (auto* b : { &muteButton, &soloButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    muteButton.setColour (juce::TextButton::buttonOnColourId, theme::warn);
    soloButton.setColour (juce::TextButton::buttonOnColourId, theme::play);

    addAndMakeVisible (meter);
    meter.setProvider ([this] { return track >= 0 ? state.getTrackLevel (track) : state.getOutputLevel (0); });

    if (track >= 0)
    {
        fader.onValueChange = [this] { state.setTrackVolume (track, (float) fader.getValue()); };
        muteButton.onClick = [this] { state.setTrackMute (track, ! muteButton.getToggleState()); };
        soloButton.onClick = [this] { state.setTrackSolo (track, ! soloButton.getToggleState()); };
        pan.onValueChange = [this] { state.setTrackPan (track, (float) pan.getValue()); };
    }
    else
    {
        // Master: Main bus (PA) only. Click, guide and in-ear monitor are never affected.
        fader.onValueChange = [this] { state.setMasterVolume ((float) fader.getValue()); };
        pan.setVisible (false);
        // M -> LIM (Main limiter on/off), S -> CLIP (latched over-0 dBFS warning, click clears).
        muteButton.setButtonText ("LIM");
        muteButton.setClickingTogglesState (false);
        muteButton.setColour (juce::TextButton::buttonOnColourId, theme::ok.darker (0.3f));
        muteButton.setTooltip ("PA limiter (-0.3 dBFS ceiling). Locked in Live Mode.");
        muteButton.onClick = [this] { state.setLimiterEnabled (! state.isLimiterEnabled()); syncFromState(); };
        soloButton.setButtonText ("CLIP");
        soloButton.setColour (juce::TextButton::buttonOnColourId, theme::danger);
        soloButton.setTooltip ("Lights when the PA mix went over 0 dBFS. Click to clear.");
        soloButton.onClick = [this] { state.clearClipLatches(); };
    }

    syncFromState();
}

void ChannelStrip::syncFromState()
{
    if (track < 0)
    {
        fader.setValue (state.getMasterVolumeDb(), juce::dontSendNotification);
        muteButton.setToggleState (state.isLimiterEnabled(), juce::dontSendNotification);
        muteButton.setEnabled (! state.isLiveMode());
        soloButton.setToggleState (state.isPAClipLatched(), juce::dontSendNotification);
        return;
    }
    const auto* song = state.getCurrentSong();
    if (song == nullptr || ! juce::isPositiveAndBelow (track, (int) song->tracks.size()))
        return;
    const auto& t = song->tracks[(size_t) track];
    fader.setValue (t.volumeDb, juce::dontSendNotification);
    pan.setValue (t.pan, juce::dontSendNotification);
    muteButton.setToggleState (t.mute, juce::dontSendNotification);
    soloButton.setToggleState (t.solo, juce::dontSendNotification);
    const auto c = colourForTrackKind (t.kind);
    fader.setColour (juce::Slider::trackColourId, c);
    pan.setColour (juce::Slider::rotarySliderFillColourId, c);
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (2, 0);
    g.setColour (track < 0 ? theme::panelRaised : theme::panel.brighter (0.02f));
    g.fillRoundedRectangle (r.toFloat(), 4.0f);

    const auto* song = state.getCurrentSong();
    juce::String name = "MASTER", route = "MAIN BUS";
    if (track < 0)
    {
        const float gr = state.getLimiterReductionDb();
        route = ! state.isLimiterEnabled() ? juce::String ("LIMITER OFF")
                : gr < -0.05f ? "GR " + juce::String (gr, 1) + " dB" : juce::String ("LIMITER 0.0");
    }
    juce::Colour c = theme::accent;
    if (track >= 0 && song != nullptr && juce::isPositiveAndBelow (track, (int) song->tracks.size()))
    {
        const auto& t = song->tracks[(size_t) track];
        name = t.name;
        route = state.getTrackDestination (track);
        c = colourForTrackKind (t.kind);
    }

    g.setColour (c);
    g.fillRect (r.getX() + 4, r.getY() + 2, r.getWidth() - 8, 3);

    g.setColour (theme::text);
    g.setFont (theme::font (11.0f, true));
    g.drawFittedText (name, nameArea, juce::Justification::centred, 2);
    g.setColour (theme::textDim);
    g.setFont (theme::font (9.5f));
    g.drawFittedText (route, routeArea, juce::Justification::centred, 2);
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (6, 6);
    r.removeFromTop (2);
    nameArea = r.removeFromTop (28);
    if (pan.isVisible())
        pan.setBounds (r.removeFromTop (30).withSizeKeepingCentre (30, 30));
    else
        r.removeFromTop (30);

    routeArea = r.removeFromBottom (24);
    auto buttons = r.removeFromBottom (22);
    if (muteButton.isVisible())
    {
        muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
        soloButton.setBounds (buttons.reduced (2, 0));
    }
    r.removeFromBottom (4);

    meter.setBounds (r.removeFromRight (8));
    r.removeFromRight (2);
    fader.setBounds (r);
}

//==============================================================================
MixerPanel::MixerPanel (AppState& s) : state (s)
{
    stripViewport.setViewedComponent (&stripHolder, false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);

    master = std::make_unique<ChannelStrip> (state, -1);
    addAndMakeVisible (*master);

    rebuildOutputMeters();
    state.addChangeListener (this);
    rebuildStrips();
    startTimerHz (30);
}

MixerPanel::~MixerPanel()
{
    state.removeChangeListener (this);
}

void MixerPanel::rebuildStrips()
{
    strips.clear();
    builtForSong = state.getCurrentSongIndex();
    if (const auto* song = state.getCurrentSong())
        for (int i = 0; i < (int) song->tracks.size(); ++i)
            stripHolder.addAndMakeVisible (strips.add (new ChannelStrip (state, i)));
    resized();
}

void MixerPanel::rebuildOutputMeters()
{
    outputMeters.clear();
    const int n = state.getRouting().getNumOutputs();
    outputMeterWidth = n < 4 ? outputMeterWidthWide : outputMeterWidthNormal;
    for (int i = 0; i < n; ++i)
        addAndMakeVisible (outputMeters.add (new LevelMeter ([this, i] { return state.getOutputLevel (i); })));
    resized();
    repaint();
}

void MixerPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (outputMeters.size() != state.getRouting().getNumOutputs())
        rebuildOutputMeters();

    const auto* song = state.getCurrentSong();
    if (builtForSong != state.getCurrentSongIndex() || (song != nullptr && (int) song->tracks.size() != strips.size()))
    {
        rebuildStrips();
        repaint();
        return;
    }
    for (auto* s : strips)
        s->syncFromState();
}

void MixerPanel::timerCallback()
{
    for (auto* s : strips) s->updateMeter();
    master->updateMeter();
    master->syncFromState();   // limiter GR and the clip latch change all the time
    master->repaint();
    for (auto* m : outputMeters) m->update();
}

void MixerPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panelInset);
    g.setColour (theme::outline);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());

    // Output section
    auto r = outputsArea;
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("PHYSICAL OUTPUTS", r.removeFromTop (20).reduced (8, 0), juce::Justification::centredLeft);

    const auto& routing = state.getRouting();
    for (int i = 0; i < outputMeters.size() && i < routing.getNumOutputs(); ++i)
    {
        const auto meterBounds = outputMeters[i]->getBounds();
        const auto& out = routing.getOutput (i);
        auto col = juce::Rectangle<int> (meterBounds.getCentreX() + 4 - outputMeterWidth / 2, meterBounds.getBottom() + 2, outputMeterWidth, 42);

        g.setColour (theme::text);
        g.setFont (theme::mono (11.0f, true));
        g.drawText (juce::String (i + 1), col.removeFromTop (13), juce::Justification::centred);

        g.setColour (theme::textDim);
        g.setFont (theme::font (9.0f, true));
        g.drawFittedText (juce::String (out.label).replace (" LEFT", " L").replace (" RIGHT", " R").replace ("MONITOR", "MON"),
                          col.removeFromTop (14), juce::Justification::centred, 1);

        const bool inEar = out.source && isInEarOnlyBus (out.source->kind);
        const auto tagText = out.isPA ? juce::String ("PA") : inEar ? juce::String ("IN-EAR") : juce::String ("AUX");
        const auto tagColour = out.isPA ? theme::accent : inEar ? theme::trackClick : theme::textFaint;
        auto tag = col.removeFromTop (13).withSizeKeepingCentre (40, 12);
        g.setColour (tagColour.withAlpha (0.2f));
        g.fillRoundedRectangle (tag.toFloat(), 2.0f);
        g.setColour (tagColour);
        g.setFont (theme::font (8.5f, true));
        g.drawText (tagText, tag, juce::Justification::centred);
    }

    g.setColour (routing.isSafe() ? theme::ok : theme::danger);
    g.setFont (theme::font (10.0f, true));
    g.drawText (routing.isSafe() ? "CLICK PROTECTED: no click/guide on PA" : "WARNING: click routed to PA",
                outputsArea.withTop (outputsArea.getBottom() - 18).reduced (8, 0), juce::Justification::centredLeft);
}

void MixerPanel::resized()
{
    auto r = getLocalBounds().reduced (6, 8);
    r.removeFromTop (1);

    const int numOuts = outputMeters.size();
    outputsArea = r.removeFromRight (numOuts * outputMeterWidth + 16);
    r.removeFromRight (8);

    if (master)
        master->setBounds (r.removeFromRight (stripWidth + 6));
    r.removeFromRight (6);

    stripViewport.setBounds (r);
    const int holderH = r.getHeight() - (stripHolder.getWidth() > r.getWidth() ? 8 : 0);
    stripHolder.setSize (juce::jmax (r.getWidth(), strips.size() * stripWidth), holderH);
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * stripWidth, 0, stripWidth, holderH);

    auto meters = outputsArea.reduced (8, 0).withTrimmedTop (22).withTrimmedBottom (20 + 44);
    for (int i = 0; i < numOuts; ++i)
    {
        auto col = meters.removeFromLeft (outputMeterWidth);
        outputMeters[i]->setBounds (col.withSizeKeepingCentre (14, col.getHeight()).translated (-4, 0));
    }
}

} // namespace b4nd
