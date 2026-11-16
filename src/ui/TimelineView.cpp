#include "ui/TimelineView.h"
#include "ui/Theme.h"

namespace b4nd
{

TimelineView::TimelineView (AppState& s) : state (s)
{
    state.addChangeListener (this);
}

TimelineView::~TimelineView()
{
    state.removeChangeListener (this);
}

void TimelineView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

juce::Rectangle<int> TimelineView::laneArea() const
{
    return getLocalBounds().withTrimmedLeft (headerWidth).withTrimmedTop (rulerHeight + cueHeight);
}

int TimelineView::trackHeight() const
{
    const auto* song = state.getCurrentSong();
    const int n = song ? juce::jmax (1, (int) song->tracks.size()) : 1;
    return juce::jlimit (30, 64, laneArea().getHeight() / n);
}

float TimelineView::xForSeconds (double s) const
{
    const auto* song = state.getCurrentSong();
    const auto lanes = laneArea();
    if (song == nullptr || song->lengthSeconds <= 0.0)
        return (float) lanes.getX();
    return (float) (lanes.getX() + (s / song->lengthSeconds) * (lanes.getWidth() - 8));
}

double TimelineView::secondsForX (float x) const
{
    const auto* song = state.getCurrentSong();
    const auto lanes = laneArea();
    if (song == nullptr || lanes.getWidth() <= 8)
        return 0.0;
    return juce::jlimit (0.0, song->lengthSeconds,
                         (x - lanes.getX()) / (double) (lanes.getWidth() - 8) * song->lengthSeconds);
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    const auto* song = state.getCurrentSong();
    if (song == nullptr)
        return;

    const auto bounds = getLocalBounds();
    const auto lanes = laneArea();
    const int th = trackHeight();

    // --- ruler ---
    auto ruler = bounds.withHeight (rulerHeight).withTrimmedLeft (headerWidth);
    g.setColour (theme::panel);
    g.fillRect (ruler);

    const int totalBars = song->tempo.barBeatAtBeat (song->tempo.beatAtSeconds (song->lengthSeconds)).bar + 1;
    const float pxPerBar = (float) (lanes.getWidth() - 8) / (float) juce::jmax (1, totalBars);
    const int labelEvery = pxPerBar >= 40 ? 1 : pxPerBar >= 20 ? 2 : pxPerBar >= 10 ? 4 : pxPerBar >= 5 ? 8 : 16;

    for (int bar = 0; bar <= totalBars; ++bar)
    {
        const double sec = song->tempo.secondsAtBeat (song->tempo.beatAtBar (bar));
        if (sec > song->lengthSeconds + 1e-6)
            break;
        const float x = xForSeconds (sec);
        const bool major = (bar % labelEvery) == 0;
        g.setColour (major ? theme::textFaint : theme::outline);
        g.drawVerticalLine ((int) x, (float) (ruler.getBottom() - (major ? 10 : 5)), (float) ruler.getBottom());
        if (major)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::mono (10.0f));
            g.drawText (juce::String (bar + 1), (int) x + 3, ruler.getY() + 2, 40, 14, juce::Justification::left);
        }
        // grid line through lanes
        g.setColour (theme::outline.withAlpha (major ? 0.55f : 0.2f));
        g.drawVerticalLine ((int) x, (float) lanes.getY(), (float) lanes.getBottom());
    }

    // Tempo change markers on the ruler
    for (const auto& tp : song->tempo.getTempoPoints())
    {
        if (tp.beat <= 0.0) continue;
        const float x = xForSeconds (song->tempo.secondsAtBeat (tp.beat));
        g.setColour (theme::warn);
        g.fillRect (x - 1.0f, (float) ruler.getY(), 2.0f, (float) ruler.getHeight());
        g.setFont (theme::mono (10.0f, true));
        g.drawText (juce::String (tp.bpm, 1) + " BPM", (int) x + 4, ruler.getBottom() - 13, 80, 12, juce::Justification::left);
    }

    // --- cue strip ---
    auto cues = juce::Rectangle<int> (lanes.getX(), rulerHeight, lanes.getWidth(), cueHeight);
    g.setColour (theme::panelInset);
    g.fillRect (cues);
    for (size_t i = 0; i < song->cues.size(); ++i)
    {
        const float x0 = xForSeconds (song->cues[i].seconds);
        const float x1 = i + 1 < song->cues.size() ? xForSeconds (song->cues[i + 1].seconds) : xForSeconds (song->lengthSeconds);
        auto box = juce::Rectangle<float> (x0, (float) cues.getY() + 2.0f, juce::jmax (2.0f, x1 - x0 - 2.0f), (float) cueHeight - 4.0f);
        g.setColour (theme::accent.withAlpha (i % 2 == 0 ? 0.22f : 0.14f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (11.0f, true));
        g.drawFittedText (song->cues[i].name, box.toNearestInt().reduced (5, 0), juce::Justification::centredLeft, 1);
    }

    // --- header column top-left corner ---
    auto corner = juce::Rectangle<int> (0, 0, headerWidth, rulerHeight + cueHeight);
    g.setColour (theme::panel);
    g.fillRect (corner);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("TRACKS  (" + juce::String ((int) song->tracks.size()) + ")", corner.reduced (12, 0), juce::Justification::centredLeft);

    // --- tracks ---
    for (size_t i = 0; i < song->tracks.size(); ++i)
    {
        const auto& t = song->tracks[i];
        const int y = lanes.getY() + (int) i * th;
        if (y > bounds.getBottom())
            break;

        const auto colour = colourForTrackKind (t.kind);
        auto header = juce::Rectangle<int> (0, y, headerWidth, th);
        g.setColour (i % 2 == 0 ? theme::panel : theme::panel.brighter (0.03f));
        g.fillRect (header);
        g.setColour (colour);
        g.fillRect (header.removeFromLeft (4).reduced (0, 1));

        auto hr = header.reduced (8, 4);
        auto msArea = hr.removeFromRight (44);
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawFittedText (t.name, hr.removeFromTop (hr.getHeight() / 2 + 2), juce::Justification::bottomLeft, 1);

        g.setFont (theme::font (10.0f, true));
        g.setColour (colour);
        const auto kind = nameForTrackKind (t.kind);
        const int kindW = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), kind) + 10;
        auto badge = hr.removeFromLeft (kindW).withHeight (14).translated (0, 2);
        g.drawRoundedRectangle (badge.toFloat(), 3.0f, 1.0f);
        g.drawText (kind, badge, juce::Justification::centred);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawFittedText (" > " + t.destination, hr.withHeight (18), juce::Justification::centredLeft, 1);

        // M / S indicators
        auto m = msArea.removeFromLeft (20).withSizeKeepingCentre (20, 18);
        msArea.removeFromLeft (4);
        auto s = msArea.removeFromLeft (20).withSizeKeepingCentre (20, 18);
        g.setColour (t.mute ? theme::warn : theme::panelInset);
        g.fillRoundedRectangle (m.toFloat(), 3.0f);
        g.setColour (t.solo ? theme::play : theme::panelInset);
        g.fillRoundedRectangle (s.toFloat(), 3.0f);
        g.setFont (theme::font (11.0f, true));
        g.setColour (t.mute ? theme::background : theme::textDim);
        g.drawText ("M", m, juce::Justification::centred);
        g.setColour (t.solo ? theme::background : theme::textDim);
        g.drawText ("S", s, juce::Justification::centred);

        // lane background + regions
        auto lane = juce::Rectangle<int> (lanes.getX(), y, lanes.getWidth(), th);
        g.setColour (theme::outline.withAlpha (0.5f));
        g.drawHorizontalLine (lane.getBottom() - 1, 0.0f, (float) lane.getRight());

        for (const auto& reg : t.regions)
        {
            const float x0 = xForSeconds (reg.getStart());
            const float x1 = xForSeconds (reg.getEnd());
            auto box = juce::Rectangle<float> (x0 + 1.0f, (float) y + 3.0f, juce::jmax (3.0f, x1 - x0 - 2.0f), (float) th - 6.0f);
            const float alpha = t.mute ? 0.12f : 0.32f;
            g.setColour (colour.withAlpha (alpha));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (colour.withAlpha (t.mute ? 0.35f : 0.8f));
            g.drawRoundedRectangle (box, 3.0f, 1.0f);

            if (t.kind == TrackKind::Click)
            {
                // beat ticks so the click lane reads as a click
                g.setColour (colour.withAlpha (0.55f));
                const double endBeat = song->tempo.beatAtSeconds (reg.getEnd());
                const auto qn = 4.0 / (double) song->tempo.barBeatAtBeat (0.0).denominator;
                for (double b = song->tempo.beatAtSeconds (reg.getStart()); b < endBeat; b += qn)
                {
                    const float x = xForSeconds (song->tempo.secondsAtBeat (b));
                    g.drawVerticalLine ((int) x, box.getY() + box.getHeight() * 0.35f, box.getBottom() - box.getHeight() * 0.35f);
                }
            }
        }
    }

    // --- play head ---
    const float px = xForSeconds (state.getPositionSeconds());
    g.setColour (state.isEmergencyStopped() ? theme::danger : juce::Colours::white);
    g.fillRect (px - 0.75f, (float) ruler.getY(), 1.5f, (float) bounds.getHeight());
    juce::Path tri;
    tri.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 9.0f);
    g.fillPath (tri);

    g.setColour (theme::outline);
    g.drawVerticalLine (headerWidth - 1, 0.0f, (float) bounds.getHeight());
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    const auto* song = state.getCurrentSong();
    if (song == nullptr)
        return;

    draggingPlayhead = false;

    if (e.x >= headerWidth && e.y < rulerHeight)
    {
        draggingPlayhead = true;
        state.locate (secondsForX ((float) e.x));
        return;
    }

    if (e.x >= headerWidth && e.y < rulerHeight + cueHeight)
    {
        // jump to the cue segment that was clicked
        const double t = secondsForX ((float) e.x);
        int idx = -1;
        for (size_t i = 0; i < song->cues.size(); ++i)
            if (song->cues[i].seconds <= t)
                idx = (int) i;
        if (idx >= 0)
            state.jumpToCue (idx);
        return;
    }

    if (e.x < headerWidth && e.y >= rulerHeight + cueHeight)
    {
        const int track = (e.y - (rulerHeight + cueHeight)) / trackHeight();
        if (! juce::isPositiveAndBelow (track, (int) song->tracks.size()))
            return;
        const int msLeft = headerWidth - 8 - 44;
        if (e.x >= msLeft && e.x < msLeft + 20)
            state.setTrackMute (track, ! song->tracks[(size_t) track].mute);
        else if (e.x >= msLeft + 24 && e.x < msLeft + 44)
            state.setTrackSolo (track, ! song->tracks[(size_t) track].solo);
    }
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingPlayhead)
        state.locate (secondsForX ((float) e.x));
}

} // namespace b4nd
