#pragma once

#include "app/MainComponent.h"

namespace b4nd
{

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, AppState&);

    void closeButtonPressed() override;
    MainComponent& getMainComponent() { return *content; }

private:
    MainComponent* content = nullptr;
    juce::Rectangle<int> windowedBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

} // namespace b4nd
