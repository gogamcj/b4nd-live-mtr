#include "ui/MidiEditorPanel.h"
#include "ui/Theme.h"

#include <cmath>

namespace b4nd
{

namespace
{
bool isBlackKey (int pitch)
{
    const int k = pitch % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

juce::String pitchName (int pitch)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[pitch % 12]) + juce::String (pitch / 12 - 1);
}

struct GridChoice { const char* name; std::int64_t ticks; };
const GridChoice gridChoices[] = { { "1/4", midiPpq }, { "1/8", midiPpq / 2 }, { "1/16", midiPpq / 4 }, { "1/32", midiPpq / 8 },
                                   { "1/8 T", midiPpq / 3 }, { "1/16 T", midiPpq / 6 } };
} // namespace

//==============================================================================
/** The piano roll itself: keyboard, ruler, notes on the bar grid, waveform behind, velocity lane. */
class MidiEditorPanel::Roll : public juce::Component,
                              private juce::ScrollBar::Listener
{
public:
    explicit Roll (MidiEditorPanel& p) : panel (p)
    {
        for (auto* sb : { &hBar, &vBar })
        {
            sb->addListener (this);
            sb->setAutoHide (false);
            addAndMakeVisible (sb);
        }
        setWantsKeyboardFocus (false);
    }

    static constexpr int keyW = 56, rulerH = 22, velH = 64, barW = 12;

    double pxPerBeat = 48.0;
    float rowH = 12.0f;
    double scrollBeat = 0.0;   // left edge
    int topPitch = 84;         // highest visible row

    juce::Rectangle<int> noteArea() const { return { keyW, rulerH, getWidth() - keyW - barW, getHeight() - rulerH - velH - barW }; }
    juce::Rectangle<int> velArea() const { auto n = noteArea(); return { n.getX(), n.getBottom(), n.getWidth(), velH }; }
    int visibleRows() const { return juce::jmax (1, (int) std::floor ((float) noteArea().getHeight() / rowH)); }

    float xForTick (std::int64_t t) const { return (float) noteArea().getX() + (float) (((double) t / midiPpq - scrollBeat) * pxPerBeat); }
    std::int64_t tickForX (float x) const { return (std::int64_t) std::llround ((scrollBeat + (x - (float) noteArea().getX()) / pxPerBeat) * midiPpq); }
    float yForPitch (int p) const { return (float) noteArea().getY() + (float) (topPitch - p) * rowH; }
    int pitchForY (float y) const { return juce::jlimit (0, 127, topPitch - (int) std::floor ((y - (float) noteArea().getY()) / rowH)); }

    std::int64_t snapTick (std::int64_t t) const
    {
        const auto g = panel.gridTicks;
        if (! panel.snap || g <= 0)
            return t;
        return (std::int64_t) std::llround ((double) t / (double) g) * g;
    }

    void resized() override
    {
        hBar.setBounds (keyW, getHeight() - barW, getWidth() - keyW - barW, barW);
        vBar.setBounds (getWidth() - barW, rulerH, barW, noteArea().getHeight());
        clampView();
    }

    void clampView()
    {
        const int rows = visibleRows();
        topPitch = juce::jlimit (juce::jmin (127, rows - 1), 127, topPitch);
        scrollBeat = juce::jmax (0.0, scrollBeat);
        const double total = juce::jmax (contentBeats(), scrollBeat + visibleBeats());
        hBar.setRangeLimits (0.0, total, juce::dontSendNotification);
        hBar.setCurrentRange (scrollBeat, visibleBeats(), juce::dontSendNotification);
        vBar.setRangeLimits (0.0, 128.0, juce::dontSendNotification);
        vBar.setCurrentRange (127.0 - topPitch, rows, juce::dontSendNotification);
        bgCacheKey = {};
        repaint();
    }

    double visibleBeats() const { return noteArea().getWidth() / pxPerBeat; }
    double contentBeats() const
    {
        double b = (double) panel.seq.lastTick() / midiPpq + 8.0;
        if (const auto* s = panel.song())
            b = juce::jmax (b, s->tempo.beatAtSeconds (s->lengthSeconds) + 4.0);
        return b;
    }

    /** Shows all notes (or bar 1 to 8 when there are none). */
    void fit()
    {
        const auto& notes = panel.seq.getNotes();
        if (notes.empty())
        {
            scrollBeat = 0.0;
            pxPerBeat = juce::jmax (8.0, noteArea().getWidth() / 32.0);
            topPitch = 84;
        }
        else
        {
            std::int64_t t0 = notes.front().start, t1 = 0;
            int lo = 127, hi = 0;
            for (const auto& n : notes)
            {
                t1 = juce::jmax (t1, n.end());
                lo = juce::jmin (lo, n.pitch);
                hi = juce::jmax (hi, n.pitch);
            }
            // from the start of the bar of the first note
            double b0 = (double) t0 / midiPpq;
            if (const auto* s = panel.song())
                b0 = s->tempo.beatAtBar (s->tempo.barBeatAtBeat (b0).bar);
            scrollBeat = juce::jmax (0.0, b0);
            const double span = juce::jmax (4.0, (double) t1 / midiPpq - scrollBeat + 1.0);
            pxPerBeat = juce::jlimit (4.0, 400.0, noteArea().getWidth() / span);
            const int rows = (int) ((float) noteArea().getHeight() / rowH);
            const int mid = (lo + hi) / 2;
            topPitch = juce::jlimit (0, 127, mid + rows / 2);
            if (hi - lo + 2 > rows)
                topPitch = juce::jmin (127, hi + 1);
        }
        clampView();
    }

    void zoom (double factor, float aroundX)
    {
        const double beatAt = scrollBeat + (aroundX - (float) noteArea().getX()) / pxPerBeat;
        pxPerBeat = juce::jlimit (4.0, 400.0, pxPerBeat * factor);
        scrollBeat = beatAt - (aroundX - (float) noteArea().getX()) / pxPerBeat;
        clampView();
    }

    void scrollBarMoved (juce::ScrollBar* sb, double start) override
    {
        if (sb == &hBar)
            scrollBeat = start;
        else
            topPitch = 127 - (int) std::round (start);
        clampView();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown())
        {
            zoom (w.deltaY > 0 ? 1.25 : 0.8, (float) e.x);
            return;
        }
        const bool horizontal = e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY);
        if (horizontal)
            scrollBeat -= (std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY) * visibleBeats() * 0.3;
        else
            topPitch += (int) std::round (w.deltaY * 12.0f);
        clampView();
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto* s = panel.song();
        const auto na = noteArea();
        g.fillAll (theme::panelInset);
        if (s == nullptr)
            return;

        // rows
        const int bottomPitch = juce::jmax (0, topPitch - visibleRows());
        for (int p = topPitch; p >= bottomPitch; --p)
        {
            const float y = yForPitch (p);
            g.setColour (isBlackKey (p) ? juce::Colour (0xff15181d) : juce::Colour (0xff1c2027));
            g.fillRect ((float) na.getX(), y, (float) na.getWidth(), rowH);
            if (p % 12 == 0)
            {
                g.setColour (theme::outline);
                g.drawHorizontalLine ((int) (y + rowH) - 1, (float) na.getX(), (float) na.getRight());
            }
        }

        paintWaveform (g, *s);

        // grid
        const double b0 = scrollBeat, b1 = scrollBeat + visibleBeats();
        int bar = s->tempo.barBeatAtBeat (juce::jmax (0.0, b0)).bar;
        for (int guard = 0; guard < 2000; ++guard, ++bar)
        {
            const double start = s->tempo.beatAtBar (bar);
            if (start > b1)
                break;
            const double end = s->tempo.beatAtBar (bar + 1);
            const auto bb = s->tempo.barBeatAtBeat (start);
            const double beatLen = (end - start) / juce::jmax (1, bb.numerator);
            const double sub = (double) panel.gridTicks / midiPpq;
            if (sub * pxPerBeat >= 6.0)
                for (double b = start; b < end - 1e-9; b += sub)
                {
                    g.setColour (juce::Colour (0x0effffff));
                    g.drawVerticalLine ((int) xForTick ((std::int64_t) std::llround (b * midiPpq)), (float) na.getY(), (float) velArea().getBottom());
                }
            if (beatLen * pxPerBeat >= 5.0)
                for (int k = 1; k < bb.numerator; ++k)
                {
                    g.setColour (juce::Colour (0x22ffffff));
                    g.drawVerticalLine ((int) xForTick ((std::int64_t) std::llround ((start + k * beatLen) * midiPpq)), (float) na.getY(), (float) velArea().getBottom());
                }
            g.setColour (juce::Colour (0x55ffffff));
            const float x = xForTick ((std::int64_t) std::llround (start * midiPpq));
            g.drawVerticalLine ((int) x, (float) na.getY(), (float) velArea().getBottom());
        }

        // notes
        g.saveState();
        g.reduceClipRegion (na);
        const auto color = panel.track() != nullptr && panel.track()->kind == TrackKind::MidiExternal ? theme::trackExt : theme::trackVsti;
        for (const auto& n : panel.seq.getNotes())
        {
            if (n.pitch > topPitch || n.pitch < bottomPitch)
                continue;
            const float x0 = xForTick (n.start), x1 = xForTick (n.end());
            if (x1 < (float) na.getX() || x0 > (float) na.getRight())
                continue;
            const bool sel = panel.selection.count (n.id) > 0;
            juce::Rectangle<float> r (x0, yForPitch (n.pitch) + 1.0f, juce::jmax (2.0f, x1 - x0 - 1.0f), rowH - 2.0f);
            // brighter = louder (velocity)
            g.setColour (color.withMultipliedBrightness (0.45f + 0.55f * (float) n.velocity / 127.0f));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (sel ? juce::Colours::white : color.darker (0.6f));
            g.drawRoundedRectangle (r, 2.0f, sel ? 1.6f : 1.0f);
        }
        if (rubber.has_value())
        {
            g.setColour (theme::accent.withAlpha (0.18f));
            g.fillRect (*rubber);
            g.setColour (theme::accent);
            g.drawRect (*rubber, 1.0f);
        }
        g.restoreState();

        // velocity lane
        const auto va = velArea();
        g.setColour (theme::panel);
        g.fillRect (va);
        g.setColour (theme::outline);
        g.drawHorizontalLine (va.getY(), (float) va.getX(), (float) va.getRight());
        for (const auto& n : panel.seq.getNotes())
        {
            const float x = xForTick (n.start);
            if (x < (float) va.getX() || x > (float) va.getRight())
                continue;
            const float h = (float) (va.getHeight() - 6) * (float) n.velocity / 127.0f;
            g.setColour (panel.selection.count (n.id) ? juce::Colours::white : color);
            g.fillRect (x, (float) va.getBottom() - h, 2.0f, h);
            g.fillEllipse (x - 2.0f, (float) va.getBottom() - h - 2.0f, 5.0f, 5.0f);
        }
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.0f));
        g.drawText ("VELOCITY", 4, va.getY() + 2, keyW - 6, 14, juce::Justification::centredLeft);

        // keyboard
        for (int p = topPitch; p >= bottomPitch; --p)
        {
            const float y = yForPitch (p);
            if (y >= (float) na.getBottom())
                continue;
            g.setColour (isBlackKey (p) ? juce::Colour (0xff202329) : juce::Colour (0xffd8dbe0));
            g.fillRect (0.0f, y, (float) keyW - 1.0f, rowH - 1.0f);
            if (p % 12 == 0 && rowH >= 8.0f)
            {
                g.setColour (juce::Colour (0xff30343b));
                g.setFont (theme::font (juce::jmin (11.0f, rowH - 1.0f)));
                g.drawText (pitchName (p), 2, (int) y, keyW - 6, (int) rowH, juce::Justification::centredRight);
            }
        }
        g.setColour (theme::panel);
        g.fillRect (0, 0, getWidth(), rulerH);
        g.setColour (theme::outline);
        g.drawHorizontalLine (rulerH - 1, 0.0f, (float) getWidth());
        // bar numbers again over the ruler background
        {
            int b = s->tempo.barBeatAtBeat (juce::jmax (0.0, b0)).bar;
            double lastX = -1000.0;
            for (int guard = 0; guard < 2000; ++guard, ++b)
            {
                const double start = s->tempo.beatAtBar (b);
                if (start > b1)
                    break;
                const float x = xForTick ((std::int64_t) std::llround (start * midiPpq));
                if (x < (float) na.getX() - 1.0f || x - lastX < 34.0)
                    continue;
                lastX = x;
                g.setColour (theme::textFaint);
                g.drawVerticalLine ((int) x, (float) rulerH - 8.0f, (float) rulerH);
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.0f));
                g.drawText (juce::String (b + 1), (int) x + 3, 3, 40, rulerH - 6, juce::Justification::centredLeft);
            }
        }
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.0f));
        g.drawText ("BAR", 4, 3, keyW - 8, rulerH - 6, juce::Justification::centredLeft);

        // play head (only when this song is the one playing)
        const auto* cur = panel.state.getCurrentSong();
        if (cur != nullptr && cur->id == s->id)
        {
            const double beat = s->tempo.beatAtSeconds (panel.state.getPositionSeconds());
            const float x = xForTick ((std::int64_t) std::llround (beat * midiPpq));
            if (x >= (float) na.getX() && x <= (float) na.getRight())
            {
                g.setColour (juce::Colours::white);
                g.fillRect (x - 0.75f, 0.0f, 1.5f, (float) va.getBottom());
            }
        }
    }

    /** Peak per pixel column of the background track, cached while the view stays the same. */
    void paintWaveform (juce::Graphics& g, const SongInfo& s)
    {
        const TrackInfo* bg = nullptr;
        for (const auto& t : s.tracks)
            if (t.id == panel.backgroundId)
                bg = &t;
        if (bg == nullptr || bg->media == nullptr || bg->mediaRate <= 0.0 || bg->media->numFrames <= 0)
            return;
        const auto na = noteArea();
        const auto key = juce::String::toHexString ((juce::pointer_sized_int) bg->media.get()) + "/" + juce::String (pxPerBeat, 4) + "/"
                         + juce::String (scrollBeat, 6) + "/" + juce::String (na.getWidth()) + "/" + juce::String (bg->mediaStartSeconds, 6)
                         + "/" + juce::String (s.tempo.secondsAtBeat (1.0), 6);
        if (key != bgCacheKey)
        {
            bgCacheKey = key;
            bgPeaks.assign ((size_t) na.getWidth(), 0.0f);
            const auto& d = *bg->media;
            for (int c = 0; c < na.getWidth(); ++c)
            {
                const double beatA = scrollBeat + c / pxPerBeat, beatB = scrollBeat + (c + 1) / pxPerBeat;
                auto i0 = (std::int64_t) ((s.tempo.secondsAtBeat (beatA) - bg->mediaStartSeconds) * bg->mediaRate);
                auto i1 = (std::int64_t) ((s.tempo.secondsAtBeat (beatB) - bg->mediaStartSeconds) * bg->mediaRate);
                i0 = juce::jlimit<std::int64_t> (0, d.numFrames, i0);
                i1 = juce::jlimit<std::int64_t> (0, d.numFrames, i1);
                const auto step = juce::jmax<std::int64_t> (1, (i1 - i0) / 512);   // bounded work per column
                float peak = 0.0f;
                for (int ch = 0; ch < juce::jmin (2, d.numChannels); ++ch)
                {
                    const float* x = d.channel (ch);
                    for (auto i = i0; i < i1; i += step)
                        peak = juce::jmax (peak, std::abs (x[i]));
                }
                bgPeaks[(size_t) c] = juce::jmin (1.0f, peak);
            }
        }
        const float mid = (float) na.getCentreY(), half = (float) na.getHeight() * 0.45f;
        g.setColour (theme::trackGuide.withAlpha (0.22f));
        for (int c = 0; c < (int) bgPeaks.size(); ++c)
            if (bgPeaks[(size_t) c] > 0.0005f)
            {
                const float h = juce::jmax (1.0f, bgPeaks[(size_t) c] * half);
                g.fillRect ((float) (na.getX() + c), mid - h, 1.0f, 2.0f * h);
            }
    }

    //==========================================================================
    enum class Drag { None, Move, Resize, Rubber, Velocity };

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = Drag::None;
        changed = false;
        rubber.reset();
        const auto na = noteArea();
        if (panel.song() == nullptr || panel.state.isLiveMode())
            return;
        if (e.y < rulerH && e.x >= keyW)
        {
            // click on the ruler: move the play head there (this song only)
            const auto* cur = panel.state.getCurrentSong();
            if (cur != nullptr && cur->id == panel.songId)
                panel.state.locate (cur->tempo.secondsAtBeat (juce::jmax (0.0, (double) tickForX ((float) e.x) / midiPpq)));
            return;
        }
        if (e.x < keyW && e.y >= na.getY() && e.y < na.getBottom())
        {
            // a key: select every note of that pitch
            const int p = pitchForY ((float) e.y);
            NoteSelection s = e.mods.isShiftDown() ? panel.selection : NoteSelection {};
            for (const auto& n : panel.seq.getNotes())
                if (n.pitch == p)
                    s.insert (n.id);
            panel.selection = s;
            panel.refreshControls();
            return;
        }
        base = panel.seq;
        downTick = tickForX ((float) e.x);
        downPitch = pitchForY ((float) e.y);

        if (velArea().contains (e.getPosition()))
        {
            velNote = 0;
            float best = 6.0f;
            for (const auto& n : panel.seq.getNotes())
            {
                const float d = std::abs (xForTick (n.start) - (float) e.x);
                if (d <= best)
                {
                    best = d;
                    velNote = n.id;
                }
            }
            if (velNote != 0)
            {
                if (! panel.selection.count (velNote))
                    panel.selection = { velNote };
                drag = Drag::Velocity;
                mouseDrag (e);
            }
            return;
        }
        if (! na.contains (e.getPosition()))
            return;

        const auto hit = panel.seq.noteAt (downTick, downPitch);
        if (hit != 0)
        {
            const auto* n = panel.seq.find (hit);
            if (e.mods.isShiftDown())
            {
                if (panel.selection.count (hit)) panel.selection.erase (hit); else panel.selection.insert (hit);
            }
            else if (! panel.selection.count (hit))
            {
                panel.selection = { hit };
            }
            if (panel.selection.count (hit))
            {
                anchorStart = n->start;
                anchorEnd = n->end();
                drag = (xForTick (n->end()) - (float) e.x <= 6.0f && xForTick (n->end()) - xForTick (n->start) > 12.0f) ? Drag::Resize : Drag::Move;
            }
            panel.refreshControls();
            return;
        }
        if (! e.mods.isShiftDown())
            panel.selection.clear();
        rubberStart = panel.selection;
        drag = Drag::Rubber;
        panel.refreshControls();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag == Drag::None)
            return;
        const auto tick = tickForX ((float) e.x);
        if (drag == Drag::Move)
        {
            const bool free = e.mods.isAltDown();
            auto dt = tick - downTick;
            if (! free)
                dt = snapTick (anchorStart + dt) - anchorStart;
            const int dp = pitchForY ((float) e.y) - downPitch;
            auto s = base;
            s.move (panel.selection, dt, dp);
            changed = dt != 0 || dp != 0;
            panel.seq = s;
        }
        else if (drag == Drag::Resize)
        {
            auto newEnd = anchorEnd + (tick - downTick);
            if (! e.mods.isAltDown())
                newEnd = snapTick (newEnd);
            auto s = base;
            s.resize (panel.selection, newEnd - anchorEnd);
            changed = newEnd != anchorEnd;
            panel.seq = s;
        }
        else if (drag == Drag::Velocity)
        {
            const auto va = velArea();
            const int v = juce::jlimit (1, 127, (int) std::round (127.0f * (float) (va.getBottom() - e.y) / (float) (va.getHeight() - 6)));
            auto s = base;
            s.setVelocity (panel.selection, v);
            changed = true;
            panel.seq = s;
            panel.setMessage ("Velocity " + juce::String (v), false);
        }
        else if (drag == Drag::Rubber)
        {
            rubber = juce::Rectangle<float> (e.mouseDownPosition, e.position).getIntersection (noteArea().toFloat());
            auto sel = rubberStart;
            const auto inside = panel.seq.notesIn (downTick, tick, downPitch, pitchForY ((float) e.y));
            sel.insert (inside.begin(), inside.end());
            panel.selection = sel;
            panel.refreshControls();
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if ((drag == Drag::Move || drag == Drag::Resize || drag == Drag::Velocity) && changed)
        {
            const auto edited = panel.seq;
            panel.seq = base;
            panel.commit (edited, panel.selection);
        }
        drag = Drag::None;
        rubber.reset();
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (panel.state.isLiveMode() || ! noteArea().contains (e.getPosition()))
            return;
        const auto tick = tickForX ((float) e.x);
        const int pitch = pitchForY ((float) e.y);
        auto s = panel.seq;
        if (const auto hit = s.noteAt (tick, pitch); hit != 0)
        {
            s.remove ({ hit });
            NoteSelection sel = panel.selection;
            sel.erase (hit);
            panel.commit (s, sel);
            return;
        }
        EditNote n;
        n.start = panel.snap ? (tick / panel.gridTicks) * panel.gridTicks : tick;   // the grid cell clicked
        n.length = panel.gridTicks;
        n.pitch = pitch;
        n.velocity = lastVelocity();
        n.channel = defaultChannel();
        const auto id = s.add (n);
        panel.commit (s, { id });
    }

    int lastVelocity() const
    {
        int v = 100;
        for (const auto& n : panel.seq.getNotes())
            if (panel.selection.count (n.id))
                v = n.velocity;
        return v;
    }

    int defaultChannel() const
    {
        const auto& notes = panel.seq.getNotes();
        return notes.empty() ? 0 : notes.front().channel;
    }

    MidiEditorPanel& panel;
    juce::ScrollBar hBar { false }, vBar { true };
    Drag drag = Drag::None;
    NoteSequence base;
    NoteSelection rubberStart;
    std::optional<juce::Rectangle<float>> rubber;
    std::int64_t downTick = 0, anchorStart = 0, anchorEnd = 0;
    int downPitch = 0;
    std::uint32_t velNote = 0;
    bool changed = false;
    juce::String bgCacheKey;
    std::vector<float> bgPeaks;
};

//==============================================================================
MidiEditorPanel::MidiEditorPanel (AppState& s) : state (s)
{
    roll = std::make_unique<Roll> (*this);
    addAndMakeVisible (*roll);

    titleLabel.setFont (theme::font (15.0f, true));
    titleLabel.setColour (juce::Label::textColourId, theme::text);
    addAndMakeVisible (titleLabel);

    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        gridBox.addItem (gridChoices[i].name, i + 1);
    gridBox.setSelectedId (3, juce::dontSendNotification);
    gridBox.setTooltip ("Grid: snap, quantize and the length of new notes");
    gridBox.onChange = [this] { setGridTicks (gridChoices[gridBox.getSelectedId() - 1].ticks); };
    snapButton.setToggleState (true, juce::dontSendNotification);
    snapButton.setTooltip ("Notes snap to the grid when moved, resized or added (hold Alt for free movement)");
    snapButton.onClick = [this] { snap = snapButton.getToggleState(); };
    backgroundBox.setTooltip ("Audio track drawn behind the notes");
    backgroundBox.onChange = [this]
    {
        const int i = backgroundBox.getSelectedId() - 2;
        const auto* sg = song();
        setBackgroundTrack (sg != nullptr && juce::isPositiveAndBelow (i, (int) sg->tracks.size()) ? sg->tracks[(size_t) i].id : juce::String());
    };

    quantizeButton.setTooltip ("Moves the selected note starts to the grid (Q)");
    quantizeButton.onClick = [this] { quantizeSelection(); };
    velDownButton.onClick = [this] { changeVelocity (-10); };
    velUpButton.onClick = [this] { changeVelocity (10); };
    octDownButton.onClick = [this] { transposeSelection (-12); };
    downButton.onClick = [this] { transposeSelection (-1); };
    upButton.onClick = [this] { transposeSelection (1); };
    octUpButton.onClick = [this] { transposeSelection (12); };
    for (auto* b : { &octDownButton, &downButton, &upButton, &octUpButton })
        b->setTooltip ("Transpose the selection (Up / Down, Shift: octave)");
    deleteButton.setTooltip ("Delete the selected notes (Delete). Double-click a note to delete it, an empty place to add one.");
    deleteButton.onClick = [this] { deleteSelection(); };
    undoButton.setTooltip ("Ctrl+Z");
    undoButton.onClick = [this] { undo(); };
    redoButton.setTooltip ("Ctrl+Y");
    redoButton.onClick = [this] { redo(); };
    zoomOutButton.onClick = [this] { roll->zoom (0.8, (float) roll->noteArea().getCentreX()); };
    zoomInButton.onClick = [this] { roll->zoom (1.25, (float) roll->noteArea().getCentreX()); };
    fitButton.onClick = [this] { roll->fit(); };
    revertButton.setTooltip ("Drops the edits since the last save");
    revertButton.onClick = [this] { revert(); };
    saveButton.setTooltip ("Writes a new MIDI file for the track (the old one stays on disk). Not while playing.");
    saveButton.onClick = [this]
    {
        if (auto err = save(); err.isNotEmpty())
            setMessage (err, true);
    };

    for (juce::Component* c : { (juce::Component*) &gridBox, (juce::Component*) &snapButton, (juce::Component*) &backgroundBox,
                                (juce::Component*) &quantizeButton, (juce::Component*) &velDownButton, (juce::Component*) &velUpButton,
                                (juce::Component*) &octDownButton, (juce::Component*) &downButton, (juce::Component*) &upButton,
                                (juce::Component*) &octUpButton, (juce::Component*) &deleteButton, (juce::Component*) &undoButton,
                                (juce::Component*) &redoButton, (juce::Component*) &zoomOutButton, (juce::Component*) &zoomInButton,
                                (juce::Component*) &fitButton, (juce::Component*) &revertButton, (juce::Component*) &saveButton })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }
    saveButton.setColour (juce::TextButton::buttonColourId, theme::accent.darker (0.3f));

    setWantsKeyboardFocus (true);
    setSize (1180, 720);
    state.addChangeListener (this);
    startTimerHz (30);
}

MidiEditorPanel::~MidiEditorPanel()
{
    state.removeChangeListener (this);
}

const SongInfo* MidiEditorPanel::song() const { return state.findSong (songId); }

const TrackInfo* MidiEditorPanel::track() const
{
    if (const auto* s = song())
        for (const auto& t : s->tracks)
            if (t.id == trackId)
                return &t;
    return nullptr;
}

juce::String MidiEditorPanel::getTrackName() const
{
    const auto* t = track();
    return t != nullptr ? t->name : juce::String();
}

juce::String MidiEditorPanel::openTrack (const juce::String& sId, const juce::String& tId)
{
    const auto* s = state.findSong (sId);
    if (s == nullptr)
        return "No such song";
    const TrackInfo* t = nullptr;
    for (const auto& x : s->tracks)
        if (x.id == tId)
            t = &x;
    if (t == nullptr || (t->kind != TrackKind::MidiVsti && t->kind != TrackKind::MidiExternal))
        return "Not a MIDI track";
    if (t->midiEvents == nullptr)
        return t->name + ": no MIDI to edit (drop a .mid file on the track, or convert a stem with Audio to MIDI)";

    songId = sId;
    trackId = tId;
    loadedPath = t->midiPath;
    fileChangedUnderneath = false;
    seq = NoteSequence::fromEvents (*t->midiEvents);
    selection.clear();
    history.reset (seq);

    // Waveform behind: the stem the part came from, else the first audio track with media.
    backgroundId.clear();
    for (const auto& x : s->tracks)
        if (t->aiPart.isNotEmpty() && x.stemName == t->aiPart && x.media != nullptr)
            backgroundId = x.id;
    if (backgroundId.isEmpty())
        for (const auto& x : s->tracks)
            if (x.kind == TrackKind::Audio && x.media != nullptr && ! x.isStem())
            {
                backgroundId = x.id;
                break;
            }
    fillBackgroundBox();
    setMessage ({}, false);
    roll->fit();
    refreshControls();
    return {};
}

void MidiEditorPanel::fillBackgroundBox()
{
    backgroundBox.clear (juce::dontSendNotification);
    backgroundBox.addItem ("No waveform", 1);
    if (const auto* s = song())
        for (int i = 0; i < (int) s->tracks.size(); ++i)
        {
            const auto& t = s->tracks[(size_t) i];
            if (t.media != nullptr)
            {
                backgroundBox.addItem ("Waveform: " + t.name, i + 2);
                if (t.id == backgroundId)
                    backgroundBox.setSelectedId (i + 2, juce::dontSendNotification);
            }
        }
    if (backgroundBox.getSelectedId() == 0)
        backgroundBox.setSelectedId (1, juce::dontSendNotification);
}

void MidiEditorPanel::setBackgroundTrack (const juce::String& id)
{
    backgroundId = id;
    fillBackgroundBox();
    roll->clampView();
}

void MidiEditorPanel::setGridTicks (std::int64_t t)
{
    gridTicks = juce::jmax<std::int64_t> (minNoteTicks, t);
    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        if (gridChoices[i].ticks == gridTicks)
            gridBox.setSelectedId (i + 1, juce::dontSendNotification);
    roll->repaint();
}

void MidiEditorPanel::commit (const NoteSequence& edited, const NoteSelection& newSelection)
{
    if (state.isLiveMode())
        return;
    const NoteSelection keep = newSelection;   // may be `selection` itself
    seq = edited;
    selection.clear();
    for (auto id : keep)
        if (seq.find (id) != nullptr)
            selection.insert (id);
    history.push (seq);
    refreshControls();
}

void MidiEditorPanel::selectAll()
{
    selection = seq.all();
    refreshControls();
}

void MidiEditorPanel::selectNone()
{
    selection.clear();
    refreshControls();
}

void MidiEditorPanel::quantizeSelection()
{
    auto s = seq;
    const auto& sel = selection.empty() ? seq.all() : selection;   // nothing selected: the whole part
    s.quantize (sel, gridTicks);
    commit (s, selection);
    setMessage ("Quantized " + juce::String ((int) sel.size()) + " notes to " + gridBox.getText(), false);
}

void MidiEditorPanel::transposeSelection (int semitones)
{
    if (selection.empty())
        return;
    auto s = seq;
    s.move (selection, 0, semitones);
    commit (s, selection);
}

void MidiEditorPanel::moveSelection (std::int64_t ticks)
{
    if (selection.empty())
        return;
    auto s = seq;
    s.move (selection, ticks, 0);
    commit (s, selection);
}

void MidiEditorPanel::changeVelocity (int delta)
{
    if (selection.empty())
        return;
    auto s = seq;
    s.changeVelocity (selection, delta);
    commit (s, selection);
}

int MidiEditorPanel::deleteSelection()
{
    if (selection.empty())
        return 0;
    auto s = seq;
    const int n = s.remove (selection);
    commit (s, {});
    setMessage ("Deleted " + juce::String (n) + " notes", false);
    return n;
}

void MidiEditorPanel::undo()
{
    if (! history.canUndo())
        return;
    seq = history.undo();
    refreshControls();
}

void MidiEditorPanel::redo()
{
    if (! history.canRedo())
        return;
    seq = history.redo();
    refreshControls();
}

void MidiEditorPanel::revert()
{
    while (history.isModified() && history.canUndo())
        seq = history.undo();
    if (history.isModified())   // the saved state is ahead (redo branch)
        while (history.isModified() && history.canRedo())
            seq = history.redo();
    refreshControls();
}

juce::String MidiEditorPanel::save()
{
    if (track() == nullptr)
        return "The track is no longer in the song";
    if (auto lock = state.midiEditLockReason(); lock.isNotEmpty())
        return lock;
    const auto err = state.saveMidiEdit (songId, trackId, seq.toEvents());
    if (err.isNotEmpty())
        return err;
    history.markSaved();
    if (const auto* t = track())
        loadedPath = t->midiPath;
    fileChangedUnderneath = false;
    setMessage ("Saved: " + juce::String (seq.size()) + " notes -> " + juce::File (loadedPath).getParentDirectory().getFileName() + "/"
                + juce::File (loadedPath).getFileName(), false);
    refreshControls();
    return {};
}

void MidiEditorPanel::setMessage (const juce::String& text, bool isError)
{
    message = text;
    messageIsError = isError;
    repaint (statusArea);
}

void MidiEditorPanel::refreshControls()
{
    const auto* t = track();
    const bool live = state.isLiveMode();
    const auto lock = state.midiEditLockReason();
    titleLabel.setText (t != nullptr ? "MIDI EDITOR  " + t->name : "MIDI EDITOR  (no track)", juce::dontSendNotification);
    const bool any = ! selection.empty();
    for (auto* b : { &velDownButton, &velUpButton, &octDownButton, &downButton, &upButton, &octUpButton, &deleteButton })
        b->setEnabled (! live && any && t != nullptr);
    quantizeButton.setEnabled (! live && t != nullptr && seq.size() > 0);
    undoButton.setEnabled (! live && history.canUndo());
    redoButton.setEnabled (! live && history.canRedo());
    revertButton.setEnabled (! live && history.isModified());
    saveButton.setEnabled (t != nullptr && lock.isEmpty() && history.isModified());
    saveButton.setTooltip (lock.isNotEmpty() ? lock : "Writes a new MIDI file for the track (the old one stays on disk). Not while playing.");
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName ("MIDI Editor - " + (t != nullptr ? t->name : juce::String ("no track")) + (history.isModified() ? " *" : ""));
    roll->repaint();
    repaint (statusArea);
}

void MidiEditorPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (state.isLiveMode())
    {
        // Never on stage: the editor closes; the draft is kept for after the show.
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>(); w != nullptr && w->isVisible())
            w->setVisible (false);
    }
    const auto* t = track();
    if (t != nullptr && t->midiPath != loadedPath)
    {
        if (! history.isModified() && t->midiEvents != nullptr)
            openTrack (songId, trackId);   // converted again / another file dropped: show the new one
        else
            fileChangedUnderneath = true;
    }
    refreshControls();
}

void MidiEditorPanel::timerCallback()
{
    // play head
    const auto* cur = state.getCurrentSong();
    if (cur != nullptr && cur->id == songId && state.getTransportState() != TransportState::Stopped)
        roll->repaint();
    const bool locked = state.midiEditLockReason().isNotEmpty();
    if (locked != wasLocked)
    {
        wasLocked = locked;
        refreshControls();
    }
}

bool MidiEditorPanel::keyPressed (const juce::KeyPress& key)
{
    if (state.isLiveMode())
        return false;
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
    if (mods.isCommandDown() && (code == 'A' || code == 'a')) { selectAll(); return true; }
    if (mods.isCommandDown() && (code == 'Z' || code == 'z')) { if (mods.isShiftDown()) redo(); else undo(); return true; }
    if (mods.isCommandDown() && (code == 'Y' || code == 'y')) { redo(); return true; }
    if (! mods.isCommandDown() && (code == 'Q' || code == 'q')) { quantizeSelection(); return true; }
    if (code == juce::KeyPress::upKey) { transposeSelection (mods.isShiftDown() ? 12 : 1); return true; }
    if (code == juce::KeyPress::downKey) { transposeSelection (mods.isShiftDown() ? -12 : -1); return true; }
    if (code == juce::KeyPress::leftKey) { moveSelection (-gridTicks); return true; }
    if (code == juce::KeyPress::rightKey) { moveSelection (gridTicks); return true; }
    if (code == juce::KeyPress::escapeKey) { selectNone(); return true; }
    if (code == juce::KeyPress::spaceKey) { state.togglePlayPause(); return true; }
    return false;
}

void MidiEditorPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawHorizontalLine (statusArea.getY() - 1, 0.0f, (float) getWidth());
    juce::String left;
    if (const auto* t = track())
    {
        left << seq.size() << " notes";
        if (! selection.empty())
            left << ", " << (int) selection.size() << " selected";
        if (history.isModified())
            left << "  |  UNSAVED EDITS";
        if (t->aiPart.isNotEmpty() && t->aiConfidence >= 0.0f)
            left << "  |  AI part " << t->aiPart << " (" << juce::roundToInt (t->aiConfidence * 100.0f) << "%)";
    }
    const auto lock = state.midiEditLockReason();
    juce::String right = message;
    bool err = messageIsError;
    if (fileChangedUnderneath)
    {
        right = "The track's MIDI changed since this draft was opened; SAVE replaces it with the draft";
        err = true;
    }
    else if (lock.isNotEmpty() && history.isModified())
    {
        right = lock;
        err = false;
    }
    auto r = statusArea.reduced (10, 0);
    g.setFont (theme::font (12.5f));
    g.setColour (history.isModified() ? theme::warn : theme::textDim);
    g.drawText (left, r.removeFromLeft (r.getWidth() / 2), juce::Justification::centredLeft);
    g.setColour (err ? theme::danger : theme::textDim);
    g.drawText (right, r, juce::Justification::centredRight);
}

void MidiEditorPanel::resized()
{
    auto r = getLocalBounds();
    statusArea = r.removeFromBottom (26);
    // Row 1: what is edited, what is drawn behind, view, save. Row 2: the editing tools.
    auto bar = r.removeFromTop (38).reduced (8, 5);
    titleLabel.setBounds (bar.removeFromLeft (juce::jmin (420, bar.getWidth() / 3)));
    saveButton.setBounds (bar.removeFromRight (80));
    bar.removeFromRight (4);
    revertButton.setBounds (bar.removeFromRight (72));
    bar.removeFromRight (16);
    fitButton.setBounds (bar.removeFromRight (44));
    bar.removeFromRight (2);
    zoomInButton.setBounds (bar.removeFromRight (28));
    bar.removeFromRight (2);
    zoomOutButton.setBounds (bar.removeFromRight (28));
    bar.removeFromRight (16);
    backgroundBox.setBounds (bar.removeFromRight (juce::jmin (320, bar.getWidth())));

    bar = r.removeFromTop (34).reduced (8, 3);
    auto place = [&bar] (juce::Component& c, int w, int gap = 4) { c.setBounds (bar.removeFromLeft (w)); bar.removeFromLeft (gap); };
    place (gridBox, 80);
    place (snapButton, 66);
    place (quantizeButton, 86, 16);
    place (velDownButton, 54);
    place (velUpButton, 54, 16);
    place (octDownButton, 40);
    place (downButton, 34);
    place (upButton, 34);
    place (octUpButton, 40, 16);
    place (deleteButton, 70, 16);
    place (undoButton, 56);
    place (redoButton, 56);
    roll->setBounds (r);
}

//==============================================================================
namespace
{
class MidiEditorWindow : public juce::DocumentWindow
{
public:
    explicit MidiEditorWindow (AppState& s)
        : DocumentWindow ("MIDI Editor - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MidiEditorPanel (s), true);
        setResizable (true, false);
        setResizeLimits (980, 480, 2600, 1600);
        centreWithSize (getWidth(), getHeight());
    }
    // Closing keeps the draft: opening the same track again shows it.
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<MidiEditorWindow> midiEditorOwner;

void openIn (MidiEditorPanel& p, AppState& state, const juce::String& songId, const juce::String& trackId)
{
    if (auto err = p.openTrack (songId, trackId); err.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "MIDI Editor", err);
        return;
    }
    juce::ignoreUnused (state);
    midiEditorOwner->setVisible (true);
    midiEditorOwner->toFront (true);
    p.grabKeyboardFocus();
}
} // namespace

void showMidiEditor (AppState& state, int trackIndex)
{
    if (state.isLiveMode())
        return;
    const auto* s = state.getCurrentSong();
    if (s == nullptr || ! juce::isPositiveAndBelow (trackIndex, (int) s->tracks.size()))
        return;
    const auto songId = s->id;
    const auto trackId = s->tracks[(size_t) trackIndex].id;
    if (midiEditorOwner == nullptr)
        midiEditorOwner = std::make_unique<MidiEditorWindow> (state);
    auto* p = getMidiEditorContent();
    if (p == nullptr)
        return;
    if (p->getSongId() == songId && p->getTrackId() == trackId && p->hasUnsavedEdits())
    {
        midiEditorOwner->setVisible (true);   // the same track with its draft
        midiEditorOwner->toFront (true);
        return;
    }
    if (p->hasUnsavedEdits())
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "MIDI Editor",
                                            "\"" + p->getTrackName() + "\" has unsaved edits. Discard them and open \""
                                                + s->tracks[(size_t) trackIndex].name + "\"?",
                                            "Discard", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create ([&state, songId, trackId] (int ok) {
                                                if (ok != 0 && midiEditorOwner != nullptr)
                                                    if (auto* panel = getMidiEditorContent())
                                                        openIn (*panel, state, songId, trackId);
                                            }));
        return;
    }
    openIn (*p, state, songId, trackId);
}

MidiEditorPanel* getMidiEditorContent()
{
    return midiEditorOwner != nullptr ? dynamic_cast<MidiEditorPanel*> (midiEditorOwner->getContentComponent()) : nullptr;
}

void closeMidiEditor()
{
    midiEditorOwner.reset();
}

} // namespace b4nd
