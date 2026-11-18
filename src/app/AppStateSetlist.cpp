// Setlist editing (Phase 13).

#include "app/AppState.h"

namespace b4nd
{

juce::String AppState::guardSetlistChange() const
{
    if (liveMode)
        return "Live Mode: the setlist cannot be changed during a show";
    if (transport == TransportState::Playing)
        return "Stop playback first";
    return {};
}

juce::String AppState::addSong (const juce::String& title, int atIndex)
{
    if (auto g = guardSetlistChange(); g.isNotEmpty())
        return g;
    if (songs.size() >= 200)
        return "A setlist holds at most 200 songs";
    SongInfo s;
    s.id = juce::String (newUuid());
    s.title = title.trim().isNotEmpty() ? title.trim() : "Song " + juce::String ((int) songs.size() + 1);
    s.lengthSeconds = 180.0;
    s.tempo.setTempoPoints ({ { 0.0, 120.0 } });
    s.tempo.setTimeSigPoints ({ { 0, 4, 4 } });
    TrackInfo click;
    click.id = juce::String (newUuid());
    click.name = "Click";
    click.kind = TrackKind::Click;
    click.destination = "OUT 3";
    click.volumeDb = -6.0f;
    click.regions = { { 0.0, s.lengthSeconds } };
    s.tracks.push_back (click);

    const int at = juce::isPositiveAndBelow (atIndex, (int) songs.size() + 1) ? atIndex : (int) songs.size();
    const auto currentId = getCurrentSong() != nullptr ? getCurrentSong()->id : juce::String();
    songs.insert (songs.begin() + at, std::move (s));
    if (currentSong >= at && currentId.isNotEmpty())
        ++currentSong;   // the same song stays selected
    if (currentSong < 0)
        currentSong = 0;
    juce::Logger::writeToLog ("Setlist: added \"" + songs[(size_t) at].title + "\" at " + juce::String (at + 1));
    markDirty();
    requestAllMedia();
    rebuildPlan();
    sendChangeMessage();
    return {};
}

juce::String AppState::removeSong (int index)
{
    if (auto g = guardSetlistChange(); g.isNotEmpty())
        return g;
    if (! juce::isPositiveAndBelow (index, (int) songs.size()))
        return "No such song";
    if (songs.size() <= 1)
        return "The setlist needs at least one song";
    const bool wasCurrent = index == currentSong;
    if (wasCurrent)
        stop();
    const auto title = songs[(size_t) index].title;
    songs.erase (songs.begin() + index);
    if (index < currentSong || currentSong >= (int) songs.size())
        --currentSong;
    currentSong = juce::jlimit (0, (int) songs.size() - 1, currentSong);
    if (wasCurrent)
        position = 0.0;
    juce::Logger::writeToLog ("Setlist: removed \"" + title + "\" (its media files stay on disk)");
    markDirty();
    requestAllMedia();
    rebuildPlan();
    mediaPool.purgeUnused();   // its audio, unless another song uses the file (or the engine still holds it)
    sendChangeMessage();
    return {};
}

juce::String AppState::moveSong (int from, int to)
{
    if (auto g = guardSetlistChange(); g.isNotEmpty())
        return g;
    const int n = (int) songs.size();
    if (! juce::isPositiveAndBelow (from, n))
        return "No such song";
    to = juce::jlimit (0, n - 1, to);
    if (from == to)
        return {};
    const auto currentId = songs[(size_t) currentSong].id;
    auto s = std::move (songs[(size_t) from]);
    songs.erase (songs.begin() + from);
    songs.insert (songs.begin() + to, std::move (s));
    for (int i = 0; i < n; ++i)
        if (songs[(size_t) i].id == currentId)
            currentSong = i;   // the selected song stays selected
    juce::Logger::writeToLog ("Setlist: moved \"" + songs[(size_t) to].title + "\" from " + juce::String (from + 1) + " to " + juce::String (to + 1));
    markDirty();
    requestAllMedia();   // the next song may be another one now
    sendChangeMessage();
    return {};
}

juce::String AppState::setSongInfo (int index, const juce::String& title, const juce::String& artist)
{
    if (liveMode)
        return "Live Mode: the setlist cannot be changed during a show";
    if (! juce::isPositiveAndBelow (index, (int) songs.size()))
        return "No such song";
    if (title.trim().isEmpty())
        return "A song needs a title";
    auto& s = songs[(size_t) index];
    if (s.title == title.trim() && s.artist == artist.trim())
        return {};
    s.title = title.trim();
    s.artist = artist.trim();
    markDirty();
    sendChangeMessage();
    return {};
}

std::vector<double> AppState::getSongStartTimes() const
{
    std::vector<double> r;
    double t = 0.0;
    for (const auto& s : songs)
    {
        r.push_back (t);
        t += s.lengthSeconds;
    }
    r.push_back (t);   // the end of the show
    return r;
}

} // namespace b4nd
