#include "ui/ProjectCommands.h"

namespace b4nd::projectCommands
{

namespace
{
std::unique_ptr<juce::FileChooser> chooser;

void showError (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message);
}

juce::File startFolder (AppState& state)
{
    if (state.hasProjectFile())
        return state.getProjectFolder().getParentDirectory();
    auto f = AppState::getDefaultProjectsFolder();
    f.createDirectory();
    return f;
}

juce::File asPackageFolder (const juce::File& chosen)
{
    return chosen.hasFileExtension (ProjectPackage::extension) ? chosen
                                                                : chosen.withFileExtension (ProjectPackage::extension);
}
} // namespace

void newProject (AppState& state)
{
    chooser = std::make_unique<juce::FileChooser> ("New project: choose a name and location",
                                                   startFolder (state).getChildFile ("New Show"), "*", true);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [&state] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (chosen == juce::File())
            return;
        const auto folder = asPackageFolder (chosen);
        if (auto err = state.newProject (folder, folder.getFileNameWithoutExtension()); err.isNotEmpty())
            showError ("New project failed", err);
    });
}

void openProject (AppState& state)
{
    chooser = std::make_unique<juce::FileChooser> ("Open project (select the .b4nd folder)", startFolder (state), "*", true);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                              | juce::FileBrowserComponent::canSelectFiles,
                          [&state] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (chosen == juce::File())
            return;
        if (auto err = state.openProject (chosen); err.isNotEmpty())
            showError ("Could not open project", err);
        else
            reviewOpenedProject (state);
    });
}

void saveAs (AppState& state)
{
    const auto suggested = startFolder (state).getChildFile (state.getProjectName().removeCharacters ("\\/:*?\"<>|"));
    chooser = std::make_unique<juce::FileChooser> ("Save project as", suggested, "*", true);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [&state] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (chosen == juce::File())
            return;
        if (auto err = state.saveProjectAs (asPackageFolder (chosen)); err.isNotEmpty())
            showError ("Save failed", err);
    });
}

void save (AppState& state)
{
    if (! state.hasProjectFile())
    {
        saveAs (state);
        return;
    }
    if (auto err = state.saveProject(); err.isNotEmpty())
        showError ("Save failed", err + "\n\nThe last successful save and the backups are untouched.");
}

void reviewOpenedProject (AppState& state)
{
    // After a crash: offer to go back to the song and place where it stopped (docs/10 §4.6).
    auto offerRecovery = [&state]
    {
        const auto& r = state.getRecovery();
        if (! r.available)
            return;
        const int sec = (int) r.seconds;
        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::QuestionIcon, "Continue where it stopped?",
            "Last time the program stopped in song " + juce::String (r.song + 1) + " \"" + r.title + "\" at "
                + juce::String (sec / 60) + ":" + juce::String (sec % 60).paddedLeft ('0', 2) + ".\n\nGo back there (stopped, ready to play)?",
            "Go there", "Stay at the start", nullptr,
            juce::ModalCallbackFunction::create ([&state] (int result)
            {
                if (result == 1)
                    state.restoreRecovery();
                else
                    state.dismissRecovery();
            }));
    };
    auto warnMissing = [&state, offerRecovery]
    {
        const auto& m = state.getMissingMediaOnOpen();
        if (m.isEmpty())
        {
            offerRecovery();
            return;
        }
        juce::String list;
        for (int i = 0; i < juce::jmin (8, m.size()); ++i)
            list << "\n  " << m[i];
        if (m.size() > 8)
            list << "\n  ... and " << (m.size() - 8) << " more";
        showError ("Missing media", juce::String (m.size()) + " audio file(s) were not found. Those tracks stay silent "
                                      "and are marked MISSING on the timeline:" + list);
        offerRecovery();
    };

    if (state.lastOpenHadStaleLock() && state.getLastOpenBackups().size() > 1)
    {
        // backups[0] is the copy made just now on open; [1] is the newest from before the crash.
        const auto previous = state.getLastOpenBackups()[1];
        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::QuestionIcon, "Project was not closed cleanly",
            "B4ND LIVE MTR did not close this project last time (crash or power loss).\n\n"
            "The project is opened with its last autosave (at most a few seconds old). "
            "If something looks wrong, you can go back to the previous backup:\n"
            + juce::File (previous).getFileName(),
            "Restore backup", "Keep last autosave", nullptr,
            juce::ModalCallbackFunction::create ([&state, previous, warnMissing] (int result)
            {
                if (result == 1)   // Esc / close keeps the autosave: no action without a deliberate click
                    if (auto err = state.restoreBackup (previous); err.isNotEmpty())
                        showError ("Restore failed", err);
                warnMissing();
            }));
        return;
    }
    warnMissing();   // (also offers the recovery)
}

} // namespace b4nd::projectCommands
