#include "ui/StatusBar.h"
#include "ui/Theme.h"

namespace b4nd
{

StatusBar::StatusBar (AppState& s) : state (s)
{
    startTimerHz (2);
}

void StatusBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());

    auto r = getLocalBounds().reduced (10, 0);

    auto item = [&] (const juce::String& caption, const juce::String& value, juce::Colour dot, int width)
    {
        auto c = r.removeFromLeft (width);
        r.removeFromLeft (8);
        g.setColour (dot);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) c.getX() + 5.0f, (float) c.getCentreY() }));
        c.removeFromLeft (14);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.0f, true));
        const int capW = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), caption) + 6;
        g.drawText (caption, c.removeFromLeft (capW), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (value, c, juce::Justification::centredLeft, 1);
    };

    // Phase 1: engines are not connected yet; state is shown honestly as such.
    item ("AUDIO", "No device (audio engine: Phase 2)", theme::textFaint, 270);
    item ("SR/BUF", "-- / --", theme::textFaint, 110);
    item ("MIDI", "No ports (Phase 4)", theme::textFaint, 150);
    item ("VSTi", "0 loaded (Phase 5)", theme::textFaint, 140);
    item ("CPU", "--", theme::textFaint, 70);
    item ("XRUN", "0", theme::ok, 70);

    const auto mode = state.isLiveMode() ? juce::String ("LIVE") : juce::String ("EDIT");
    g.setColour (state.isLiveMode() ? theme::danger : theme::accent);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("MODE: " + mode + "   v" + juce::String (JUCE_APPLICATION_VERSION_STRING), r, juce::Justification::centredRight);
}

} // namespace b4nd
