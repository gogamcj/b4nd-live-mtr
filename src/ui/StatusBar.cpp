#include "ui/StatusBar.h"
#include "ui/Theme.h"

namespace b4nd
{

StatusBar::StatusBar (AppState& s) : state (s)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Click for audio device settings (Ctrl+,)");
    startTimerHz (4);
}

void StatusBar::mouseUp (const juce::MouseEvent&)
{
    if (onAudioSettingsRequested)
        onAudioSettingsRequested();
}

void StatusBar::paint (juce::Graphics& g)
{
    auto& dev = state.getDevice();
    const auto status = dev.getStatus();
    const bool running = status == AudioDeviceController::Status::Running;
    const bool lost = status == AudioDeviceController::Status::Lost || status == AudioDeviceController::Status::SavedDeviceMissing;

    g.fillAll (lost ? theme::danger.darker (0.6f) : theme::panel);
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
        g.setColour (lost ? theme::text : theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (value, c, juce::Justification::centredLeft, 1);
    };

    const auto audioDot = running ? theme::ok : lost ? theme::danger : theme::textFaint;
    item ("AUDIO", dev.getStatusText(), audioDot, 300);

    if (running)
    {
        item ("SR/BUF", juce::String (dev.getSampleRate() / 1000.0, 1) + "k / " + juce::String (dev.getBufferSize()), theme::ok, 120);
        item ("LAT", juce::String (dev.getOutputLatencyMs(), 1) + " ms", theme::ok, 80);
        const float load = state.getEngine().getLoad();
        item ("CPU", juce::String (juce::roundToInt (load * 100.0f)) + "%", load > 0.7f ? theme::warn : theme::ok, 70);
    }
    else
    {
        item ("SR/BUF", "-- / --", theme::textFaint, 120);
        item ("LAT", "--", theme::textFaint, 80);
        item ("CPU", "--", theme::textFaint, 70);
    }

    const auto drops = state.getDropoutCount();
    item ("DROPOUT", juce::String ((juce::int64) drops), drops == 0 ? theme::ok : theme::danger, 90);
    {
        const auto m = state.getMediaSummary();
        juce::String v = juce::String (juce::roundToInt ((double) m.ramBytes / (1024.0 * 1024.0))) + " MB";
        auto dot = theme::ok;
        if (m.missing + m.errors > 0) { v = juce::String (m.missing + m.errors) + " MISSING  " + v; dot = theme::danger; }
        else if (m.loading > 0)       { v = "LOADING " + juce::String (m.loading) + "  " + v;        dot = theme::warn; }
        else if (m.ramBytes > AppState::ramWarningBytes) dot = theme::warn;
        else if (m.ready == 0)        dot = theme::textFaint;
        item ("MEDIA", v, dot, 150);
    }
    item ("MIDI", "Phase 4", theme::textFaint, 90);

    const auto mode = state.isLiveMode() ? juce::String ("LIVE") : juce::String ("EDIT");
    g.setColour (state.isLiveMode() ? theme::danger : theme::accent);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("MODE: " + mode + "   v" + juce::String (JUCE_APPLICATION_VERSION_STRING), r, juce::Justification::centredRight);
}

} // namespace b4nd
