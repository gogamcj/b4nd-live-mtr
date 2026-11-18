#include "ui/SidePanel.h"
#include "core/TimeFormat.h"
#include "ui/AnalysisPanel.h"
#include "ui/FootswitchPanel.h"
#include "ui/ModulePanel.h"
#include "ui/PluginPanel.h"
#include "ui/ProjectCommands.h"
#include "ui/Theme.h"

namespace b4nd
{

SidePanel::SidePanel (AppState& s) : state (s)
{
    songList.setRowHeight (44);
    songList.setColour (juce::ListBox::backgroundColourId, theme::panel);
    songList.setWantsKeyboardFocus (false);
    addAndMakeVisible (songList);

    const char* items[] = { "Import Song (MP3/WAV)", "BPM / Beat / Key", "Chord Analysis", "Song Structure",
                            "Stem Separation", "Audio to MIDI" };
    for (auto* name : items)
    {
        auto* b = aiButtons.add (new juce::TextButton (name));
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    aiButtons[0]->setTooltip ("Adds an audio file to the current song as a new audio track (or drop it on the timeline)");
    aiButtons[0]->onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import a song (audio file)", juce::File(), "*.wav;*.mp3;*.flac;*.aif;*.aiff;*.ogg");
        juce::Component::SafePointer<SidePanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  if (auto err = safe->state.addAudioFile (fc.getResult()); err.isNotEmpty())
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import", err);
                              });
    };
    for (int i = 1; i <= 3; ++i)
    {
        aiButtons[i]->setTooltip ("AI analysis of the current song: tempo, bars, key, chords, sections (Ctrl+Shift+A). "
                                  "Results are drafts you check before applying.");
        aiButtons[i]->onClick = [this] { showAnalysisWindow (state); };
    }
    aiButtons[4]->setTooltip ("Separate the song into vocals / drums / bass / guitar / piano / other (Demucs). "
                              "The stems are added as muted tracks; the original stays.");
    aiButtons[4]->onClick = [this]
    {
        showAnalysisWindow (state);
        if (auto* p = getAnalysisWindowContent())
            p->preselectStems();
    };
    aiButtons[5]->setTooltip ("Convert the stem tracks to MIDI (Basic Pitch; drums: kick / snare / hi-hat). "
                              "The parts are added as muted MIDI tracks.");
    aiButtons[5]->onClick = [this]
    {
        showAnalysisWindow (state);
        if (auto* p = getAnalysisWindowContent())
            p->preselectMidi();
    };

    newButton.onClick  = [this] { projectCommands::newProject (state); };
    openButton.onClick = [this] { projectCommands::openProject (state); };
    saveButton.onClick = [this] { projectCommands::save (state); };
    newButton.setTooltip ("New project (Ctrl+N)");
    openButton.setTooltip ("Open project (Ctrl+O)");
    saveButton.setTooltip ("Save (Ctrl+S). Changes are also saved automatically 5 s after each edit.");
    modulesButton.onClick = [this] { showModuleWindow (state); };
    modulesButton.setTooltip ("External MIDI sound modules: ports, channels, presets, status (Ctrl+M)");
    pluginsButton.onClick = [this] { showPluginWindow (state); };
    pluginsButton.setTooltip ("VST3 plugins: scan, Live Certified status, which plugin each VSTi track plays (Ctrl+P)");
    footswitchButton.onClick = [this] { showFootswitchWindow (state); };
    footswitchButton.setTooltip ("Footswitch / MIDI control: inputs, pedal mapping, MIDI Learn (Ctrl+F)");
    for (auto* b : { &newButton, &openButton, &saveButton, &modulesButton, &pluginsButton, &footswitchButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    state.addChangeListener (this);
    songList.updateContent();
    songList.selectRow (state.getCurrentSongIndex(), true, true);
}

SidePanel::~SidePanel()
{
    state.removeChangeListener (this);
}

void SidePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (songList.getNumRowsOnScreen() >= 0 && builtFor != state.getProjectFolder().getFullPathName() + juce::String ((int) state.getSongs().size()))
    {
        builtFor = state.getProjectFolder().getFullPathName() + juce::String ((int) state.getSongs().size());
        songList.updateContent();
        songList.repaint();
        repaint();
    }
    const bool canChange = ! state.isLiveMode() && state.getTransportState() != TransportState::Playing;
    for (int i = 0; i <= 5 && i < aiButtons.size(); ++i)
        aiButtons[i]->setEnabled (! state.isLiveMode());
    newButton.setEnabled (canChange);
    openButton.setEnabled (canChange);
    saveButton.setButtonText (state.isDirty() ? "SAVE *" : "SAVE");
    {
        const auto ms = state.getMidiSummary();
        modulesButton.setButtonText (ms.modules == 0    ? juce::String ("MIDI MODULES")
                                     : ms.problems > 0  ? "MIDI MODULES - " + juce::String (ms.problems) + " NOT READY"
                                                        : "MIDI MODULES - " + juce::String (ms.ready) + "/" + juce::String (ms.modules) + " READY");
        if (ms.problems > 0)
            modulesButton.setColour (juce::TextButton::buttonColourId, theme::danger.darker (0.45f));
        else
            modulesButton.removeColour (juce::TextButton::buttonColourId);
    }

    {
        const auto& fp = state.getFootswitch().getPorts();
        int lost = 0;
        for (const auto& p : fp)
            lost += p.status != FootswitchInput::PortState::Status::Open ? 1 : 0;
        footswitchButton.setButtonText (fp.empty() ? juce::String ("FOOTSWITCH")
                                        : lost > 0 ? "FOOTSWITCH - NOT CONNECTED" : "FOOTSWITCH - READY");
        if (lost > 0)
            footswitchButton.setColour (juce::TextButton::buttonColourId, theme::danger.darker (0.45f));
        else
            footswitchButton.removeColour (juce::TextButton::buttonColourId);
    }

    {
        const auto ps = state.getPluginSummary();
        pluginsButton.setButtonText (ps.instruments == 0  ? juce::String ("PLUGINS")
                                     : ps.problems > 0    ? "PLUGINS - " + juce::String (ps.problems) + " NOT LOADED"
                                     : ps.notCertified > 0 ? "PLUGINS - " + juce::String (ps.notCertified) + " NOT CERTIFIED"
                                                           : "PLUGINS - " + juce::String (ps.ready) + " READY");
        if (ps.problems > 0)
            pluginsButton.setColour (juce::TextButton::buttonColourId, theme::danger.darker (0.45f));
        else if (ps.notCertified > 0)
            pluginsButton.setColour (juce::TextButton::buttonColourId, theme::warn.darker (0.6f));
        else
            pluginsButton.removeColour (juce::TextButton::buttonColourId);
    }

    if (songList.getSelectedRow() != state.getCurrentSongIndex())
        songList.selectRow (state.getCurrentSongIndex(), true, true);

    if (lastLive != state.isLiveMode())
    {
        lastLive = state.isLiveMode();
        repaint();
    }
}

int SidePanel::getNumRows()
{
    return (int) state.getSongs().size();
}

void SidePanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return;

    const auto& song = state.getSongs()[(size_t) row];
    auto r = juce::Rectangle<int> (0, 0, width, height).reduced (6, 2);

    if (selected)
    {
        g.setColour (theme::accent.withAlpha (0.18f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (theme::accent);
        g.fillRect (r.getX(), r.getY() + 4, 3, r.getHeight() - 8);
    }

    auto text = r.reduced (10, 4);
    g.setColour (theme::textFaint);
    g.setFont (theme::mono (12.0f, true));
    g.drawText (juce::String (row + 1).paddedLeft ('0', 2), text.removeFromLeft (24), juce::Justification::centredLeft);

    auto top = text.removeFromTop (text.getHeight() / 2);
    g.setColour (selected ? theme::text : theme::text.withAlpha (0.85f));
    g.setFont (theme::font (13.0f, true));
    g.drawFittedText (song.title, top, juce::Justification::bottomLeft, 1);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawText (juce::String (song.tempo.bpmAtBeat (0.0), 0) + " BPM  " + song.key + "  "
                    + formatSeconds (song.lengthSeconds, false),
                text, juce::Justification::topLeft);
}

void SidePanel::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    state.selectSong (row);
}

void SidePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    auto header = [&g] (juce::Rectangle<int> r, const juce::String& t)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText (t, r.reduced (12, 0), juce::Justification::centredLeft);
        g.setColour (theme::outline);
        g.drawHorizontalLine (r.getBottom() - 1, 8.0f, (float) r.getRight() - 8.0f);
    };

    header (songHeader, "SETLIST");
    header (aiHeader, state.isLiveMode() ? "AI ANALYSIS  (locked in Live Mode)" : "AI ANALYSIS");

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawText ("Setlist total  " + formatSeconds (state.getSetlistTotalSeconds(), false),
                totalArea.reduced (12, 0), juce::Justification::centredLeft);
}

void SidePanel::resized()
{
    auto r = getLocalBounds();
    songHeader = r.removeFromTop (30);
    {
        auto row = songHeader.reduced (8, 4).withTrimmedLeft (64);
        const int w = row.getWidth() / 3;
        saveButton.setBounds (row.removeFromRight (w).reduced (2, 0));
        openButton.setBounds (row.removeFromRight (w).reduced (2, 0));
        newButton.setBounds (row.reduced (2, 0));
    }

    const int n = aiButtons.size();
    const int buttonH = juce::jlimit (22, 30, (getHeight() * 2 / 5 - 40) / juce::jmax (1, n) - 4);
    auto ai = r.removeFromBottom (30 + n * (buttonH + 4) + 10);
    totalArea = r.removeFromBottom (26);
    footswitchButton.setBounds (r.removeFromBottom (34).reduced (10, 3));
    pluginsButton.setBounds (r.removeFromBottom (34).reduced (10, 3));
    modulesButton.setBounds (r.removeFromBottom (34).reduced (10, 3));
    songList.setBounds (r.reduced (0, 4));

    aiHeader = ai.removeFromTop (30);
    ai.reduce (10, 4);
    for (auto* b : aiButtons)
    {
        b->setBounds (ai.removeFromTop (buttonH));
        ai.removeFromTop (4);
    }
}

} // namespace b4nd
