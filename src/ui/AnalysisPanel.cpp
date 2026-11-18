#include "ui/AnalysisPanel.h"

#include "core/TimeFormat.h"
#include "ui/Theme.h"

namespace b4nd
{

namespace
{
juce::Colour confColour (double c)
{
    if (c < 0.0)
        return theme::textDim;
    return c < 0.7 ? theme::warn : theme::ok;
}

juce::String confText (double c)
{
    return c < 0.0 ? juce::String ("-") : juce::String (juce::roundToInt (c * 100.0)) + "%";
}

const juce::StringArray& allKeys()
{
    static const juce::StringArray k = []
    {
        juce::StringArray a;
        for (const auto* n : { "C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B" })
            for (const auto* m : { " major", " minor" })
                a.add (juce::String (n) + m);
        return a;
    }();
    return k;
}

juce::String stateText (AnalyzerRunner::State s)
{
    switch (s)
    {
        case AnalyzerRunner::State::Idle:      return "Idle";
        case AnalyzerRunner::State::Preparing: return "Preparing audio";
        case AnalyzerRunner::State::Running:   return "Analysing";
        case AnalyzerRunner::State::Done:      return "Done";
        case AnalyzerRunner::State::Failed:    return "FAILED";
        case AnalyzerRunner::State::Cancelled: return "Cancelled";
    }
    return {};
}
} // namespace

/** One section row: include toggle, editable name, position and confidence. */
class SectionRow : public juce::Component
{
public:
    explicit SectionRow (AnalysisPanel& p) : panel (p)
    {
        addAndMakeVisible (include);
        addAndMakeVisible (name);
        name.setFont (theme::font (14.0f));
        name.setColour (juce::TextEditor::backgroundColourId, theme::panelInset);
        name.setColour (juce::TextEditor::outlineColourId, theme::outline);
        include.onClick = [this] { if (valid()) panel.sections[(size_t) row].include = include.getToggleState(); };
        name.onTextChange = [this] { if (valid()) panel.sections[(size_t) row].s.label = name.getText().substring (0, 40); };
    }
    void set (int r)
    {
        row = r;
        if (! valid())
            return;
        const auto& d = panel.sections[(size_t) r];
        include.setToggleState (d.include, juce::dontSendNotification);
        if (name.getText() != d.s.label)
            name.setText (d.s.label, false);
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        if (! valid())
            return;
        const auto& d = panel.sections[(size_t) row].s;
        auto r = getLocalBounds().withTrimmedLeft (250);
        g.setFont (theme::mono (13.0f));
        g.setColour (theme::textDim);
        g.drawText ("bar " + juce::String (d.bar + 1) + "  " + juce::String (formatSeconds (d.startSec, false)) + "  " + d.section,
                    r.removeFromLeft (220), juce::Justification::centredLeft);
        g.setColour (confColour (d.confidence));
        g.drawText (confText (d.confidence), r, juce::Justification::centredLeft);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (2, 3);
        include.setBounds (r.removeFromLeft (28));
        name.setBounds (r.removeFromLeft (210));
    }

private:
    bool valid() const { return juce::isPositiveAndBelow (row, (int) panel.sections.size()); }
    AnalysisPanel& panel;
    juce::ToggleButton include;
    juce::TextEditor name;
    int row = -1;
};

AnalysisPanel::AnalysisPanel (AppState& s) : state (s)
{
    for (auto* b : { &analyzerButton, &checkButton, &analyzeButton, &cancelButton, &applyButton, &undoButton,
                     &halfButton, &doubleButton, &earlierButton, &laterButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    analyzeButton.setColour (juce::TextButton::buttonColourId, theme::accent.darker (0.4f));
    applyButton.setColour (juce::TextButton::buttonColourId, theme::play.darker (0.5f));
    for (auto* t : { &taskTempo, &taskKey, &taskChords, &taskSections, &taskStems, &taskMidi,
                     &useTempo, &useTimeSig, &useKey, &useChords, &useCues })
    {
        t->setToggleState (true, juce::dontSendNotification);
        t->setWantsKeyboardFocus (false);
        addAndMakeVisible (*t);
    }
    for (auto* t : { &taskStems, &taskMidi })
    {
        t->setToggleState (false, juce::dontSendNotification);
        t->setEnabled (false);
        t->setTooltip ("Comes with a later phase (docs/09)");
    }
    for (auto* e : { &bpmEditor, &gridEditor })
    {
        e->setFont (theme::mono (15.0f));
        e->setJustification (juce::Justification::centredLeft);
        e->setColour (juce::TextEditor::backgroundColourId, theme::panelInset);
        e->setColour (juce::TextEditor::outlineColourId, theme::outline);
        addAndMakeVisible (*e);
    }
    bpmEditor.setInputRestrictions (8, "0123456789.");
    gridEditor.setInputRestrictions (8, "0123456789.");
    bpmEditor.setTooltip ("BPM to use. Leave the analysed value to keep its tempo map (with tempo changes).");
    gridEditor.setTooltip ("Where bar 1 beat 1 is, in seconds from the start of the song");
    for (auto* c : { &sourceBox, &timeSigBox, &keyBox })
        addAndMakeVisible (*c);
    for (int n : { 2, 3, 4, 5, 6, 7 })
        timeSigBox.addItem (juce::String (n) + "/4", n);
    for (int i = 0; i < allKeys().size(); ++i)
        keyBox.addItem (allKeys()[i], i + 1);
    sectionList.setRowHeight (32);
    sectionList.setColour (juce::ListBox::backgroundColourId, theme::panelInset);
    addAndMakeVisible (sectionList);

    analyzerButton.setTooltip ("Choose the analyzer: the standalone b4nd-analyzer program, or a Python 3 with numpy and scipy");
    analyzerButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Analyzer program or python executable", juce::File(), "*");
        juce::Component::SafePointer<AnalysisPanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  safe->state.getAnalyzer().setCommand (fc.getResult().getFullPathName());
                                  safe->probeAnalyzer();
                              });
    };
    checkButton.onClick = [this] { probeAnalyzer(); };
    analyzeButton.onClick = [this]
    {
        juce::StringArray tasks;
        if (taskTempo.getToggleState()) tasks.add ("tempo");
        if (taskKey.getToggleState()) tasks.add ("key");
        if (taskChords.getToggleState()) tasks.add ("chords");
        if (taskSections.getToggleState()) tasks.add ("structure");
        if (tasks.isEmpty())
            return setMessage ("Choose at least one analysis.", true);
        const auto err = state.startAnalysis (sourceBox.getSelectedId() - 1, tasks);
        setMessage (err.isEmpty() ? "Analysing. You can keep working; the song is not changed until APPLY." : err, err.isNotEmpty());
    };
    cancelButton.onClick = [this] { state.cancelAnalysis(); };
    halfButton.onClick = [this] { tempoScale = tempoScale > 1.5 ? 1.0 : 0.5; refresh(); };
    doubleButton.onClick = [this] { tempoScale = tempoScale < 0.75 ? 1.0 : 2.0; refresh(); };
    earlierButton.onClick = [this] { gridSec = std::max (0.0, gridSec - draftBeatSeconds()); gridEditor.setText (juce::String (gridSec, 3), false); };
    laterButton.onClick = [this] { gridSec += draftBeatSeconds(); gridEditor.setText (juce::String (gridSec, 3), false); };
    gridEditor.onTextChange = [this] { gridSec = std::max (0.0, gridEditor.getText().getDoubleValue()); };
    applyButton.onClick = [this] { apply(); };
    undoButton.onClick = [this]
    {
        const auto err = state.undoAnalysisApply();
        setMessage (err.isEmpty() ? "Undone: the song is back as it was before APPLY." : err, err.isNotEmpty());
    };

    state.addChangeListener (this);
    startTimerHz (4);
    setSize (1000, 720);
    refresh();
    probeAnalyzer();
}

AnalysisPanel::~AnalysisPanel()
{
    state.removeChangeListener (this);
}

juce::String AnalysisPanel::lockReason() const
{
    if (state.isLiveMode())
        return "LIVE MODE: analysis is for preparation and is locked during a show.";
    if (state.getTransportState() != TransportState::Stopped)
        return "Stop playback to analyse or apply.";
    return {};
}

void AnalysisPanel::setMessage (const juce::String& text, bool isError)
{
    message = text;
    messageIsError = isError;
    repaint();
}

void AnalysisPanel::probeAnalyzer()
{
    // Starting Python and importing numpy / scipy takes a moment: never on the message thread.
    analyzerChecked = false;
    analyzerInfo.clear();
    analyzerError.clear();
    repaint();
    juce::Component::SafePointer<AnalysisPanel> safe (this);
    const auto cmd = state.getAnalyzer().getCommand();
    juce::Thread::launch ([safe, cmd]
    {
        juce::String error;
        auto info = AnalyzerRunner::probe (cmd, error);
        juce::MessageManager::callAsync ([safe, info, error]
        {
            if (safe == nullptr)
                return;
            safe->analyzerChecked = true;
            safe->analyzerError = error;
            safe->analyzerInfo = info;
            if (info.isNotEmpty())
            {
                juce::var v;
                if (juce::JSON::parse (info, v).wasOk())
                    safe->analyzerInfo = "Analyzer " + v["b4nd_analyzer"].toString() + " (Python " + v["python"].toString() + ", numpy "
                                         + v["numpy"].toString() + ", scipy " + v["scipy"].toString() + ")";
                juce::Logger::writeToLog ("Analyzer ready: " + safe->analyzerInfo);
            }
            else
                juce::Logger::writeToLog ("Analyzer NOT AVAILABLE: " + error);
            safe->repaint();
        });
    });
}

double AnalysisPanel::draftBeatSeconds() const
{
    double bpm = bpmEditor.getText().getDoubleValue();
    if (bpm < 20.0)
        bpm = 120.0;
    return 60.0 / bpm;
}

void AnalysisPanel::fillSourceBox()
{
    const auto* song = state.getCurrentSong();
    const int keep = sourceBox.getSelectedId();
    sourceBox.clear (juce::dontSendNotification);
    if (song == nullptr)
        return;
    for (int i = 0; i < (int) song->tracks.size(); ++i)
    {
        const auto& t = song->tracks[(size_t) i];
        if (t.kind == TrackKind::Audio || t.kind == TrackKind::Guide)
            sourceBox.addItem (t.name + (t.mediaStatus == TrackInfo::MediaStatus::Ready ? "" : "  (no audio loaded)"), i + 1);
    }
    const int def = state.findAnalysisSourceTrack();
    sourceBox.setSelectedId (keep > 0 && sourceBox.indexOfItemId (keep) >= 0 ? keep : def + 1, juce::dontSendNotification);
}

void AnalysisPanel::loadDraft (const AnalysisResult& r)
{
    tempoScale = 1.0;
    gridSec = r.gridStartSec;
    gridEditor.setText (juce::String (gridSec, 3), false);
    bpmEditor.setText (r.hasTempo ? juce::String (r.bpm, 2) : juce::String(), false);
    timeSigBox.setSelectedId (r.numerator, juce::dontSendNotification);
    keyBox.clear (juce::dontSendNotification);
    int id = 1;
    for (const auto& k : r.keyCandidates)
        keyBox.addItem (k + "  (candidate)", id++);
    keyBox.addSeparator();
    for (const auto& k : allKeys())
        keyBox.addItem (k, id++);
    keyBox.setSelectedId (r.keyCandidates.isEmpty() ? 0 : 1, juce::dontSendNotification);
    sections.clear();
    for (const auto& s : r.sections)
        sections.push_back ({ s, true });
    sectionList.updateContent();
    useTempo.setToggleState (r.hasTempo, juce::dontSendNotification);
    useTimeSig.setToggleState (r.hasTempo, juce::dontSendNotification);
    useKey.setToggleState (r.hasKey, juce::dontSendNotification);
    useChords.setToggleState (r.hasChords, juce::dontSendNotification);
    useCues.setToggleState (r.hasSections, juce::dontSendNotification);
}

void AnalysisPanel::refresh()
{
    const auto* song = state.getCurrentSong();
    const auto st = state.getAnalysisStatus();
    const auto* r = state.getAnalysisResult();
    const juce::String id = song == nullptr ? juce::String()
                                            : song->id + ":" + juce::String ((int) song->analysisRuns.size()) + ":"
                                                  + juce::String ((juce::pointer_sized_int) r);
    if (id != draftFor)
    {
        draftFor = id;
        fillSourceBox();
        if (r != nullptr)
            loadDraft (*r);
        else
        {
            sections.clear();
            sectionList.updateContent();
            bpmEditor.clear();
            gridEditor.clear();
        }
    }
    const bool busy = st.state == AnalyzerRunner::State::Preparing || st.state == AnalyzerRunner::State::Running;
    const bool locked = lockReason().isNotEmpty();
    analyzeButton.setEnabled (! busy && ! locked && song != nullptr);
    cancelButton.setEnabled (busy);
    analyzerButton.setEnabled (! busy && ! state.isLiveMode());
    const bool hasResult = r != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &applyButton, &halfButton, &doubleButton, &earlierButton, &laterButton,
                                                             &bpmEditor, &gridEditor, &timeSigBox, &keyBox, &useTempo, &useTimeSig,
                                                             &useKey, &useChords, &useCues, &sectionList })
        c->setEnabled (hasResult && ! locked && ! busy);
    undoButton.setEnabled (state.canUndoAnalysisApply() && ! locked);
    halfButton.setToggleState (tempoScale < 0.75, juce::dontSendNotification);
    doubleButton.setToggleState (tempoScale > 1.5, juce::dontSendNotification);
    if (hasResult && r->hasTempo)
    {
        // Half / double: show the scaled tempo in the field (still "the analysed map").
        const auto shown = juce::String (r->bpm * tempoScale, 2);
        if (bpmEditor.getText().getDoubleValue() > 0.0
            && (std::abs (bpmEditor.getText().getDoubleValue() - r->bpm * 2.0) < 0.01
                || std::abs (bpmEditor.getText().getDoubleValue() - r->bpm * 0.5) < 0.01
                || std::abs (bpmEditor.getText().getDoubleValue() - r->bpm) < 0.01))
            bpmEditor.setText (shown, false);
    }
    repaint();
}

void AnalysisPanel::apply()
{
    const auto* r = state.getAnalysisResult();
    if (r == nullptr)
        return;
    AppState::AnalysisApply a;
    a.withTempo = useTempo.getToggleState();
    a.withTimeSig = useTimeSig.getToggleState();
    a.withKey = useKey.getToggleState();
    a.withChords = useChords.getToggleState();
    a.withCues = useCues.getToggleState();
    a.tempoScale = tempoScale;
    const double typed = bpmEditor.getText().getDoubleValue();
    if (r->hasTempo && typed >= 20.0 && std::abs (typed - r->bpm * tempoScale) > 0.005)
        a.bpm = typed;   // a hand-typed BPM: one constant tempo
    if (std::abs (gridSec - r->gridStartSec) > 0.0005)
        a.gridStartSec = gridSec;
    if (timeSigBox.getSelectedId() > 0 && timeSigBox.getSelectedId() != r->numerator)
        a.numerator = timeSigBox.getSelectedId();
    if (keyBox.getSelectedId() > 0)
    {
        const auto k = keyBox.getText().upToFirstOccurrenceOf ("  (", false, false);
        if (k != r->key)
            a.key = k;
    }
    a.useEditedSections = true;
    for (const auto& d : sections)
        if (d.include && d.s.label.trim().isNotEmpty())
            a.sections.push_back (d.s);
    const auto err = state.applyAnalysis (a);
    setMessage (err.isEmpty() ? "Applied to the song. AI cues are marked; UNDO APPLY restores the song." : err, err.isNotEmpty());
}

juce::String AnalysisPanel::applyAll()
{
    for (auto* t : { &useTempo, &useTimeSig, &useKey, &useChords, &useCues })
        t->setToggleState (true, juce::dontSendNotification);
    apply();
    return messageIsError ? message : juce::String();
}

void AnalysisPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void AnalysisPanel::timerCallback()
{
    refresh();
}

int AnalysisPanel::getNumRows()
{
    return (int) sections.size();
}

juce::Component* AnalysisPanel::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* c = dynamic_cast<SectionRow*> (existing);
    if (c == nullptr)
    {
        delete existing;
        c = new SectionRow (*this);
    }
    c->set (row);
    return c;
}

void AnalysisPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    const auto* song = state.getCurrentSong();
    const auto st = state.getAnalysisStatus();
    const auto* r = state.getAnalysisResult();

    g.setColour (theme::text);
    g.setFont (theme::font (20.0f, true));
    g.drawText ("AI ANALYSIS" + (song != nullptr ? "  -  " + song->title : juce::String()), headerArea, juce::Justification::centredLeft);

    g.setFont (theme::font (13.0f));
    if (! analyzerChecked)
        g.setColour (theme::textDim);
    else
        g.setColour (analyzerInfo.isNotEmpty() ? theme::ok : theme::danger);
    g.drawFittedText (analyzerInfo.isNotEmpty() ? analyzerInfo : analyzerError.isNotEmpty() ? "NOT AVAILABLE: " + analyzerError
                                                                                           : juce::String ("Checking the analyzer..."),
                      analyzerArea, juce::Justification::centredLeft, 2);

    // Progress
    {
        auto p = progressArea;
        g.setColour (theme::panelInset);
        g.fillRoundedRectangle (p.toFloat(), 4.0f);
        const bool busy = st.state == AnalyzerRunner::State::Preparing || st.state == AnalyzerRunner::State::Running;
        if (busy)
        {
            g.setColour (theme::accent.withAlpha (0.6f));
            g.fillRoundedRectangle (p.withWidth ((int) (p.getWidth() * st.progress)).toFloat(), 4.0f);
        }
        juce::String t = stateText (st.state);
        if (busy)
            t << "  " << st.task << "  " << juce::roundToInt (st.progress * 100.0f) << "%  " << juce::String (st.elapsedSec, 0) << " s";
        else if (st.message.isNotEmpty())
            t << ": " << st.message;
        g.setColour (st.state == AnalyzerRunner::State::Failed ? theme::danger : theme::text);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (t, p.reduced (8, 0), juce::Justification::centredLeft, 1);
    }

    // Result details beside the controls
    g.setFont (theme::font (13.0f));
    if (r == nullptr)
    {
        g.setColour (theme::textDim);
        g.drawFittedText ("No analysis yet for this song. Results are drafts: check them before APPLY.", resultsArea,
                          juce::Justification::centred, 2);
    }
    else
    {
        auto line = [&] (juce::Rectangle<int> area, const juce::String& text, double conf)
        {
            g.setColour (theme::textDim);
            g.drawText (text, area.withTrimmedRight (60), juce::Justification::centredLeft);
            g.setColour (confColour (conf));
            g.setFont (theme::font (13.0f, true));
            g.drawText (confText (conf), area.removeFromRight (60), juce::Justification::centredRight);
            g.setFont (theme::font (13.0f));
        };
        if (r->hasTempo)
        {
            juce::String t = r->variableTempo ? juce::String ((int) r->tempoMap.size()) + " tempo sections:" : juce::String ("constant tempo");
            if (r->variableTempo)
                for (size_t i = 0; i < r->tempoMap.size() && i < 3; ++i)
                    t << "  bar " << (int) (r->tempoMap[i].beat / r->numerator) + 1 << " " << juce::String (r->tempoMap[i].bpm * tempoScale, 1);
            line (tempoInfoArea, t, r->bpmConfidence);
            g.setColour (theme::text);
            g.setFont (theme::font (13.0f, true));
            g.drawText ("BAR 1 AT (s)", barLabelArea.withTrimmedLeft (4), juce::Justification::centredLeft);
            g.setFont (theme::font (13.0f));
            g.setColour (theme::textDim);
            g.drawText ("first downbeat (analysed " + juce::String (r->gridStartSec, 3) + ")", barInfoArea, juce::Justification::centredLeft);
            line (sigInfoArea, "from bar accents", r->timeSigConfidence);
        }
        if (r->hasKey)
            line (keyInfoArea, r->keyCandidates.joinIntoString (" / "), r->keyConfidence);
        if (r->hasChords)
        {
            juce::StringArray first;
            double sum = 0.0;
            int n = 0;
            for (const auto& c : r->chords)
            {
                if (first.size() < 12)
                    first.add (juce::String::fromUTF8 (c.chord.c_str()));
                if (c.confidence >= 0.0) { sum += c.confidence; ++n; }
            }
            line (chordsArea, juce::String ((int) r->chords.size()) + ": " + first.joinIntoString (" ") + (r->chords.size() > 12 ? " ..." : ""),
                  n > 0 ? sum / n : -1.0);
        }
        if (r->hasSections)
        {
            g.setColour (theme::textDim);
            g.drawText (juce::String ((int) sections.size()) + " sections (list on the right); your own cues are kept",
                        cuesInfoArea, juce::Justification::centredLeft);
        }
        if (! r->errors.isEmpty())
        {
            g.setColour (theme::warn);
            g.drawFittedText ("Not done: " + r->errors.joinIntoString ("; "), errorsArea, juce::Justification::centredLeft, 2);
        }
    }

    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f, true));
    g.drawText ("SECTIONS -> CUES (rename, untick to leave out)", sectionsHeader, juce::Justification::centredLeft);

    if (message.isNotEmpty())
    {
        g.setColour (messageIsError ? theme::danger : theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (message, messageArea, juce::Justification::centredLeft, 2);
    }
    if (auto why = lockReason(); why.isNotEmpty())
    {
        g.setColour (theme::warn);
        g.setFont (theme::font (13.0f, true));
        g.drawText (why, lockArea, juce::Justification::centredLeft);
    }
}

void AnalysisPanel::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    headerArea = r.removeFromTop (30);
    auto a = r.removeFromTop (30);
    checkButton.setBounds (a.removeFromRight (80));
    a.removeFromRight (6);
    analyzerButton.setBounds (a.removeFromRight (120));
    a.removeFromRight (10);
    analyzerArea = a;
    r.removeFromTop (8);

    auto src = r.removeFromTop (28);
    sourceBox.setBounds (src.removeFromLeft (300));
    src.removeFromLeft (12);
    for (auto* t : { &taskTempo, &taskKey, &taskChords, &taskSections })
    {
        t->setBounds (src.removeFromLeft (t == &taskTempo ? 170 : t == &taskSections ? 140 : 80));
    }
    auto src2 = r.removeFromTop (26);
    src2.removeFromLeft (312);
    taskStems.setBounds (src2.removeFromLeft (150));
    taskMidi.setBounds (src2.removeFromLeft (200));
    r.removeFromTop (6);
    auto run = r.removeFromTop (30);
    analyzeButton.setBounds (run.removeFromLeft (140));
    run.removeFromLeft (6);
    cancelButton.setBounds (run.removeFromLeft (90));
    run.removeFromLeft (10);
    progressArea = run;
    r.removeFromTop (14);

    lockArea = r.removeFromBottom (22);
    messageArea = r.removeFromBottom (36);
    auto bottom = r.removeFromBottom (32);
    applyButton.setBounds (bottom.removeFromLeft (180));
    bottom.removeFromLeft (8);
    undoButton.setBounds (bottom.removeFromLeft (130));
    r.removeFromBottom (8);

    auto left = r.removeFromLeft (r.getWidth() / 2 + 60);
    r.removeFromLeft (16);
    resultsArea = left;
    auto row = [&left] { auto x = left.removeFromTop (28); left.removeFromTop (8); return x; };
    {
        auto x = row();
        useTempo.setBounds (x.removeFromLeft (110));
        bpmEditor.setBounds (x.removeFromLeft (90));
        x.removeFromLeft (6);
        halfButton.setBounds (x.removeFromLeft (40));
        x.removeFromLeft (4);
        doubleButton.setBounds (x.removeFromLeft (40));
        x.removeFromLeft (12);
        tempoInfoArea = x;
    }
    {
        auto x = row();
        barLabelArea = x.removeFromLeft (110);
        gridEditor.setBounds (x.removeFromLeft (90));
        x.removeFromLeft (6);
        earlierButton.setBounds (x.removeFromLeft (70));
        x.removeFromLeft (4);
        laterButton.setBounds (x.removeFromLeft (70));
        x.removeFromLeft (12);
        barInfoArea = x;
    }
    {
        auto x = row();
        useTimeSig.setBounds (x.removeFromLeft (110));
        timeSigBox.setBounds (x.removeFromLeft (90));
        x.removeFromLeft (12);
        sigInfoArea = x;
    }
    {
        auto x = row();
        useKey.setBounds (x.removeFromLeft (110));
        keyBox.setBounds (x.removeFromLeft (200));
        x.removeFromLeft (12);
        keyInfoArea = x;
    }
    {
        auto x = row();
        useChords.setBounds (x.removeFromLeft (110));
        chordsArea = x;
    }
    {
        auto x = row();
        useCues.setBounds (x.removeFromLeft (110));
        cuesInfoArea = x;
    }
    errorsArea = left.removeFromTop (40);
    sectionsHeader = r.removeFromTop (22);
    sectionList.setBounds (r);
}

//==============================================================================
namespace
{
class AnalysisWindow : public juce::DocumentWindow
{
public:
    explicit AnalysisWindow (AppState& s)
        : DocumentWindow ("AI Analysis - B4ND LIVE MTR", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AnalysisPanel (s), true);
        setResizable (true, false);
        setResizeLimits (900, 620, 1800, 1200);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

std::unique_ptr<AnalysisWindow> analysisWindowOwner;
} // namespace

void showAnalysisWindow (AppState& state)
{
    if (analysisWindowOwner == nullptr)
        analysisWindowOwner = std::make_unique<AnalysisWindow> (state);
    analysisWindowOwner->setVisible (true);
    analysisWindowOwner->toFront (true);
}

void closeAnalysisWindow()
{
    analysisWindowOwner.reset();
}

AnalysisPanel* getAnalysisWindowContent()
{
    return analysisWindowOwner != nullptr ? dynamic_cast<AnalysisPanel*> (analysisWindowOwner->getContentComponent()) : nullptr;
}

} // namespace b4nd
