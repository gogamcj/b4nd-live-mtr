#include "app/MainWindow.h"
#include "ui/Theme.h"

namespace b4nd
{

MainWindow::MainWindow (const juce::String& name, AppState& state)
    : DocumentWindow (name, theme::background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    content = new MainComponent (state);
    setContentOwned (content, true);
    setResizable (true, true);
    setResizeLimits (1280, 720, 10000, 10000);
    centreWithSize (getWidth(), getHeight());

    content->onLiveModeChanged = [this] (bool live)
    {
        // Full screen in live mode; the previous window size is restored afterwards.
        if (live)
        {
            windowedBounds = getBounds();
            setFullScreen (true);
        }
        else
        {
            setFullScreen (false);
            if (! windowedBounds.isEmpty())
                setBounds (windowedBounds);
        }
    };

    setVisible (true);
    content->grabKeyboardFocus();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace b4nd
