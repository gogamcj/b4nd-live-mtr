#include "ui/TimelineView.h"
#include "midi/MidiFileImport.h"
#include "ui/MidiEditorPanel.h"
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
    return getLocalBounds().withTrimmedLeft (headerWidth).withTrimmedTop (topHeight());
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
        const bool ai = song->cues[i].source == "ai";   // analysis draft: violet, marked "AI"
        g.setColour ((ai ? theme::trackVsti : theme::accent).withAlpha (i % 2 == 0 ? 0.24f : 0.15f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (11.0f, true));
        g.drawFittedText (song->cues[i].name + (ai ? "  AI" : ""), box.toNearestInt().reduced (5, 0), juce::Justification::centredLeft, 1);
    }

    // --- chord strip (analysis draft: shown, never played) ---
    if (const int ch = chordHeight(); ch > 0)
    {
        auto strip = juce::Rectangle<int> (lanes.getX(), rulerHeight + cueHeight, lanes.getWidth(), ch);
        g.setColour (theme::panelInset.darker (0.2f));
        g.fillRect (strip);
        g.setFont (theme::mono (10.0f, true));
        float lastTextEnd = -1.0e9f;
        for (const auto& c : song->chords)
        {
            const float x0 = xForSeconds (c.startSec), x1 = xForSeconds (c.endSec);
            if (x1 < (float) strip.getX() || x0 > (float) strip.getRight())
                continue;
            const auto name = juce::String::fromUTF8 (c.chord.c_str());
            const bool sure = c.confidence < 0.0 || c.confidence >= 0.7;
            g.setColour (theme::outline);
            g.drawVerticalLine ((int) x0, (float) strip.getY() + 2.0f, (float) strip.getBottom() - 2.0f);
            if (name == "N" || x0 < lastTextEnd)
                continue;   // no label where it would overlap the previous one
            g.setColour (sure ? theme::textDim : theme::warn);
            g.drawText (name, (int) x0 + 3, strip.getY(), juce::jmax (24, (int) (x1 - x0) - 4), ch, juce::Justification::centredLeft);
            lastTextEnd = x0 + 6.0f + (float) name.length() * 7.0f;
        }
    }

    // Lead-in before bar 1 (the music starts later than the file): shaded on the ruler.
    if (const double off = song->tempo.getOffsetSeconds(); off > 0.0)
    {
        const float x = xForSeconds (off);
        g.setColour (theme::background.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> ((float) lanes.getX(), 0.0f, juce::jmax (0.0f, x - (float) lanes.getX()), (float) rulerHeight));
    }

    // --- header column top-left corner ---
    auto corner = juce::Rectangle<int> (0, 0, headerWidth, topHeight());
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
        if (t.isStem() || t.aiPart.isNotEmpty())
        {
            // AI stem / AI MIDI: violet badge with its confidence, "!" when it needs a listen (docs/07).
            const juce::String ai = "AI " + (t.aiConfidence >= 0.0f ? juce::String (juce::roundToInt (t.aiConfidence * 100.0f)) + "%"
                                                                    : juce::String());
            const int aiW = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), ai) + 10;
            hr.removeFromLeft (3);
            auto b = hr.removeFromLeft (aiW).withHeight (14).translated (0, 2);
            g.setColour (theme::trackVsti);
            g.fillRoundedRectangle (b.toFloat(), 3.0f);
            g.setColour (theme::background);
            g.drawText (ai, b, juce::Justification::centred);
            if (t.aiWarning.isNotEmpty())
            {
                hr.removeFromLeft (3);
                auto w = hr.removeFromLeft (14).withHeight (14).translated (0, 2);
                g.setColour (theme::warn);
                g.fillRoundedRectangle (w.toFloat(), 3.0f);
                g.setColour (theme::background);
                g.drawText ("!", w, juce::Justification::centred);
            }
        }
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawFittedText (" > " + state.getTrackDestination ((int) i), hr.withHeight (18), juce::Justification::centredLeft, 1);

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

        const bool isMediaTrack = t.kind == TrackKind::Audio || t.kind == TrackKind::Guide;
        const bool isExtMidi = t.kind == TrackKind::MidiExternal;
        if ((isMediaTrack && t.mediaStatus != TrackInfo::MediaStatus::Ready)
            || (isExtMidi && t.midiStatus != TrackInfo::MidiStatus::Ready))
        {
            // Audio lanes show what is really loaded: no demo regions pretending to be audio.
            juce::String label;
            auto c = theme::textFaint;
            if (isExtMidi)
            {
                switch (t.midiStatus)
                {
                    case TrackInfo::MidiStatus::None:    label = "NO MIDI FILE  -  drop a .mid file here"; break;
                    case TrackInfo::MidiStatus::Missing: label = "MISSING FILE  " + t.midiPath; c = theme::danger; break;
                    case TrackInfo::MidiStatus::Error:   label = "MIDI FILE ERROR  " + t.midiError; c = theme::danger; break;
                    case TrackInfo::MidiStatus::Ready:   break;
                }
            }
            else switch (t.mediaStatus)
            {
                case TrackInfo::MediaStatus::None:    label = "NO MEDIA  -  drop an audio file here"; break;
                case TrackInfo::MediaStatus::Loading: label = "LOADING  " + juce::File (t.mediaPath).getFileName(); c = theme::textDim; break;
                case TrackInfo::MediaStatus::Missing: label = "MISSING FILE  " + t.mediaPath; c = theme::danger; break;
                case TrackInfo::MediaStatus::Error:   label = "LOAD ERROR  " + t.mediaError; c = theme::danger; break;
                case TrackInfo::MediaStatus::Unloaded: label = "NOT IN RAM (loads when the song is next)  " + juce::File (t.mediaPath).getFileName(); c = theme::textDim; break;
                case TrackInfo::MediaStatus::Ready:   break;
            }
            auto box = lane.reduced (2, 3).toFloat();
            g.setColour (c.withAlpha (0.08f));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (c.withAlpha (0.6f));
            const float dash[] = { 4.0f, 4.0f };
            juce::Path p;
            p.addRoundedRectangle (box, 3.0f);
            juce::Path dashed;
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dash, 2);
            g.fillPath (dashed);
            g.setColour (c);
            g.setFont (theme::font (11.0f, true));
            g.drawFittedText (label, box.toNearestInt().reduced (10, 0), juce::Justification::centredLeft, 1);
            continue;
        }

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

            if (isMediaTrack && t.overview != nullptr && ! t.overview->empty() && box.getWidth() > 4.0f)
            {
                // Waveform overview (peak per pixel column).
                const auto& ov = *t.overview;
                const float mid = box.getCentreY(), half = box.getHeight() * 0.45f;
                g.setColour (colour.withAlpha (t.mute ? 0.3f : 0.85f));
                for (int px = 0; px < (int) box.getWidth(); ++px)
                {
                    const auto b0 = (size_t) ((double) px / box.getWidth() * (double) ov.size());
                    const auto b1 = std::max (b0 + 1, (size_t) ((double) (px + 1) / box.getWidth() * (double) ov.size()));
                    float pk = 0.0f;
                    for (size_t b = b0; b < b1 && b < ov.size(); ++b)
                        pk = std::max (pk, ov[b]);
                    const float h = std::max (0.5f, std::min (1.0f, pk) * half);
                    g.fillRect (box.getX() + (float) px, mid - h, 1.0f, 2.0f * h);
                }
            }

            if ((isExtMidi || t.kind == TrackKind::MidiVsti) && t.midiEvents != nullptr && box.getWidth() > 4.0f)
            {
                // Note blocks: pitch range of the file mapped to the lane height.
                int lo = 127, hi = 0;
                for (const auto& e : *t.midiEvents)
                    if (e.msg.isNoteOn()) { lo = juce::jmin (lo, (int) e.msg.data[1]); hi = juce::jmax (hi, (int) e.msg.data[1]); }
                if (hi >= lo)
                {
                    const float span = (float) juce::jmax (12, hi - lo + 1);
                    std::array<std::int64_t, 128> onAt;
                    onAt.fill (-1);
                    g.setColour (colour.withAlpha (t.mute ? 0.3f : 0.9f));
                    for (const auto& e : *t.midiEvents)
                    {
                        const int n = e.msg.data[1] & 0x7f;
                        if (e.msg.isNoteOn())
                            onAt[(size_t) n] = e.tick;
                        else if (e.msg.isNoteOff() && onAt[(size_t) n] >= 0)
                        {
                            const float xa = xForSeconds (song->tempo.secondsAtBeat ((double) onAt[(size_t) n] / midiPpq));
                            const float xb = xForSeconds (song->tempo.secondsAtBeat ((double) e.tick / midiPpq));
                            const float yy = box.getBottom() - 3.0f - ((float) (n - lo) + 1.0f) / span * (box.getHeight() - 6.0f);
                            g.fillRect (xa, yy, juce::jmax (1.0f, xb - xa), juce::jmax (1.5f, (box.getHeight() - 6.0f) / span));
                            onAt[(size_t) n] = -1;
                        }
                    }
                }
            }

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

    // --- drag-and-drop feedback ---
    if (dropTrack >= -1)
    {
        g.setColour (theme::accent.withAlpha (0.25f));
        if (dropTrack >= 0)
            g.fillRect (juce::Rectangle<int> (0, lanes.getY() + dropTrack * th, bounds.getWidth(), th));
        else
        {
            const int y = lanes.getY() + (int) song->tracks.size() * th;
            auto r = juce::Rectangle<int> (0, y, bounds.getWidth(), juce::jmax (th, bounds.getBottom() - y));
            g.fillRect (r);
            g.setColour (theme::text);
            g.setFont (theme::font (13.0f, true));
            g.drawText (dropIsMidi ? "+ ADD EXTERNAL MIDI TRACK" : "+ ADD AUDIO TRACK", r.withHeight (th), juce::Justification::centred);
        }
    }
    if (notice.isNotEmpty() && juce::Time::getMillisecondCounter() - noticeTime < 6000)
    {
        auto r = bounds.withTrimmedLeft (headerWidth).removeFromBottom (28).reduced (8, 3);
        g.setColour ((noticeIsError ? theme::danger : theme::panelRaised).withAlpha (0.92f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (juce::Colours::white);
        g.setFont (theme::font (12.0f, true));
        g.drawFittedText (notice, r.reduced (8, 0), juce::Justification::centredLeft, 1);
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

    if (e.x >= headerWidth && e.y < topHeight())
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

    if (e.x < headerWidth && e.y >= topHeight())
    {
        const int track = (e.y - (topHeight())) / trackHeight();
        if (! juce::isPositiveAndBelow (track, (int) song->tracks.size()))
            return;
        const int msLeft = headerWidth - 8 - 44;
        if (e.x >= msLeft && e.x < msLeft + 20)
            state.setTrackMute (track, ! song->tracks[(size_t) track].mute);
        else if (e.x >= msLeft + 24 && e.x < msLeft + 44)
            state.setTrackSolo (track, ! song->tracks[(size_t) track].solo);
    }
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto* song = state.getCurrentSong();
    const int track = trackAtY (e.y);
    if (song == nullptr || track < 0 || state.isLiveMode())
        return;
    const auto& t = song->tracks[(size_t) track];
    if (t.kind != TrackKind::MidiVsti && t.kind != TrackKind::MidiExternal)
        return;
    if (t.midiEvents == nullptr)
    {
        showNotice (t.name + ": no MIDI yet. Drop a .mid file on it, or convert a stem with Audio to MIDI.", false);
        return;
    }
    showMidiEditor (state, track);
}

int TimelineView::trackAtY (int y) const
{
    const auto* song = state.getCurrentSong();
    const int top = topHeight();
    if (song == nullptr || y < top)
        return -1;
    const int t = (y - top) / trackHeight();
    return t < (int) song->tracks.size() ? t : -1;
}

void TimelineView::showNotice (const juce::String& text, bool isError)
{
    notice = text;
    noticeIsError = isError;
    noticeTime = juce::Time::getMillisecondCounter();
    repaint();
    juce::Component::SafePointer<TimelineView> safe (this);
    juce::Timer::callAfterDelay (6100, [safe] { if (safe != nullptr) safe->repaint(); });
}

bool TimelineView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (state.canReadAudioFile (juce::File (f)) || isMidiFile (juce::File (f)))
            return true;
    return false;
}

void TimelineView::fileDragMove (const juce::StringArray& files, int, int y)
{
    const bool midiDrop = ! files.isEmpty() && isMidiFile (juce::File (files[0]));
    dropIsMidi = midiDrop;
    int t = trackAtY (y);
    if (t >= 0)
    {
        const auto kind = state.getCurrentSong()->tracks[(size_t) t].kind;
        // A lane of the other kind gets a new track instead.
        if (midiDrop ? (kind != TrackKind::MidiExternal && kind != TrackKind::MidiVsti) : (kind != TrackKind::Audio && kind != TrackKind::Guide))
            t = -1;
    }
    if (t != dropTrack)
    {
        dropTrack = t;
        repaint();
    }
}

void TimelineView::fileDragExit (const juce::StringArray&)
{
    dropTrack = -2;
    repaint();
}

void TimelineView::filesDropped (const juce::StringArray& files, int x, int y)
{
    fileDragMove (files, x, y);
    const int target = dropTrack;
    dropTrack = -2;

    int added = 0;
    juce::String lastError;
    bool first = true;
    for (const auto& path : files)
    {
        const juce::File f (path);
        const bool midiFile = isMidiFile (f);
        if (! midiFile && ! state.canReadAudioFile (f))
            continue;
        // The first file goes to the lane under the cursor; any others become new tracks.
        const auto err = midiFile ? state.addMidiFile (f, first ? target : -1) : state.addAudioFile (f, first ? target : -1);
        first = false;
        if (err.isEmpty()) ++added;
        else               lastError = err;
    }
    if (lastError.isNotEmpty())
        showNotice (lastError, true);
    else if (added > 0 && state.getMidiSummary().tracksWithoutModule > 0)
        showNotice ("MIDI file loaded. No sound module assigned yet: open MIDI MODULES (Ctrl+M).", true);
    else if (added > 0 && dropIsMidi)
        showNotice ("MIDI file loaded: " + juce::String (added) + " file(s)", false);
    else if (added > 0)
        showNotice ("Loading " + juce::String (added) + " file(s) into memory...", false);
    repaint();
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingPlayhead)
        state.locate (secondsForX ((float) e.x));
}

} // namespace b4nd
