// Stage actions and footswitch settings (Phase 12).

#include "app/AppState.h"

namespace b4nd
{

void AppState::jumpToPreviousCue()
{
    const auto* s = getCurrentSong();
    if (s == nullptr)
        return;
    int idx = -1;
    for (size_t i = 0; i < s->cues.size(); ++i)
        if (s->cues[i].seconds <= position + 0.05)
            idx = (int) i;
    // just after a cue start: go to the one before (like a "previous track" button)
    if (idx >= 0 && position - s->cues[(size_t) idx].seconds < 1.5)
        --idx;
    locate (idx >= 0 ? s->cues[(size_t) idx].seconds : 0.0);
}

juce::String AppState::performLiveAction (LiveAction a, bool fromFootswitch)
{
    juce::String refused;
    const bool playing = transport == TransportState::Playing;
    switch (a)
    {
        case LiveAction::PlayPause:     togglePlayPause(); break;
        case LiveAction::Stop:          stop(); break;
        case LiveAction::EmergencyStop: emergencyStop(); break;
        case LiveAction::NextCue:       jumpToNextCue(); break;
        case LiveAction::PreviousCue:   jumpToPreviousCue(); break;
        case LiveAction::ToggleLoop:    toggleLoop(); break;
        case LiveAction::Restart:
            if (fromFootswitch && playing) refused = "stop the song first";
            else restart();
            break;
        case LiveAction::NextSong:
            if (fromFootswitch && playing) refused = "stop the song first";
            else if (getNextSong() == nullptr) refused = "last song";
            else nextSong();
            break;
        case LiveAction::PreviousSong:
            if (fromFootswitch && playing) refused = "stop the song first";
            else if (currentSong <= 0) refused = "first song";
            else previousSong();
            break;
        case LiveAction::None:
        case LiveAction::count:
            return {};
    }
    if (fromFootswitch)
    {
        const auto& in = footswitch->getLastPress().input;
        lastFootswitchText = juce::String (liveActionLabel (a)) + " (" + (in.note ? "Note " : "CC ") + juce::String (in.number) + ")"
                             + (refused.isNotEmpty() ? " refused: " + refused : juce::String());
        lastFootswitchMs = juce::Time::getMillisecondCounterHiRes();
        juce::Logger::writeToLog ("Footswitch: " + lastFootswitchText);
        sendChangeMessage();
    }
    return refused;
}

juce::File AppState::getFootswitchSettingsFile()
{
    return getLastProjectRecordFile().getParentDirectory().getChildFile ("footswitch.json");
}

void AppState::setFootswitchPorts (const std::vector<MidiPortInfo>& p)
{
    footswitch->setPorts (p);
    saveFootswitchSettings();
    sendChangeMessage();
}

void AppState::setFootswitchBindings (const std::vector<FootswitchBinding>& b)
{
    footswitch->setBindings (b);
    saveFootswitchSettings();
    sendChangeMessage();
}

void AppState::saveFootswitchSettings()
{
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> ports, binds;
    for (const auto& p : footswitch->getPorts())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", p.port.name);
        o->setProperty ("id", p.port.identifier);
        ports.add (juce::var (o));
    }
    for (const auto& b : footswitch->getBindings())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("port", juce::String (b.port));
        o->setProperty ("type", b.note ? "note" : "cc");
        o->setProperty ("channel", b.channel < 0 ? 0 : b.channel + 1);   // 0 = any, 1-16
        o->setProperty ("number", b.number);
        o->setProperty ("action", juce::String (liveActionId (b.action)));
        o->setProperty ("doublePress", b.doublePress);
        o->setProperty ("latching", b.latching);
        binds.add (juce::var (o));
    }
    root->setProperty ("version", 1);
    root->setProperty ("ports", ports);
    root->setProperty ("bindings", binds);
    const auto f = getFootswitchSettingsFile();
    f.getParentDirectory().createDirectory();
    const auto tmp = f.getSiblingFile ("footswitch.json.tmp");
    if (! tmp.replaceWithText (juce::JSON::toString (juce::var (root))) || ! tmp.moveFileTo (f))
        juce::Logger::writeToLog ("Footswitch settings NOT SAVED: " + f.getFullPathName());
}

void AppState::loadFootswitchSettings()
{
    const auto f = getFootswitchSettingsFile();
    if (! f.existsAsFile())
        return;   // defaults (docs/04 §8), no input enabled
    const auto v = juce::JSON::parse (f.loadFileAsString());
    if (! v.isObject())
    {
        juce::Logger::writeToLog ("Footswitch settings unreadable, using the defaults: " + f.getFullPathName());
        return;
    }
    std::vector<FootswitchBinding> binds;
    if (const auto* arr = v["bindings"].getArray())
        for (const auto& x : *arr)
        {
            FootswitchBinding b;
            b.port = x["port"].toString().toStdString();
            b.note = x["type"].toString() == "note";
            b.channel = juce::jlimit (0, 16, (int) x["channel"]) - 1;
            b.number = (int) x["number"];
            b.action = liveActionFromId (x["action"].toString().toStdString());
            b.doublePress = (bool) x["doublePress"];
            b.latching = (bool) x["latching"];
            binds.push_back (b);
        }
    if (v.hasProperty ("bindings"))
        footswitch->setBindings (binds);
    std::vector<MidiPortInfo> ports;
    if (const auto* arr = v["ports"].getArray())
        for (const auto& x : *arr)
            ports.push_back ({ x["name"].toString(), x["id"].toString() });
    footswitch->setPorts (ports);
    juce::Logger::writeToLog ("Footswitch: " + juce::String ((int) ports.size()) + " inputs, "
                              + juce::String ((int) footswitch->getBindings().size()) + " bindings");
}

} // namespace b4nd
