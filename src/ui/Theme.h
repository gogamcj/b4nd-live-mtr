#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace b4nd::theme
{
// Dark DAW palette. All UI colours come from here so a high-contrast stage theme
// can be added later by swapping these values.
inline const juce::Colour background   { 0xff121418 };
inline const juce::Colour panel        { 0xff1a1d23 };
inline const juce::Colour panelRaised  { 0xff22262e };
inline const juce::Colour panelInset   { 0xff0e1013 };
inline const juce::Colour outline      { 0xff2d323b };
inline const juce::Colour text         { 0xffe7e9ec };
inline const juce::Colour textDim      { 0xff8a919c };
inline const juce::Colour textFaint    { 0xff5a616c };

inline const juce::Colour accent       { 0xff3fa9f5 };
inline const juce::Colour play         { 0xff2fbf68 };
inline const juce::Colour pause        { 0xfff0b429 };
inline const juce::Colour danger       { 0xffe5484d };
inline const juce::Colour warn         { 0xfff0b429 };
inline const juce::Colour ok           { 0xff2fbf68 };

inline const juce::Colour trackAudio   { 0xff4d8fd6 };
inline const juce::Colour trackVsti    { 0xffb07cf2 };
inline const juce::Colour trackExt     { 0xffe08a3c };
inline const juce::Colour trackClick   { 0xffe5484d };
inline const juce::Colour trackGuide   { 0xff27bfae };

inline const juce::Colour meterLow     { 0xff2fbf68 };
inline const juce::Colour meterMid     { 0xfff0b429 };
inline const juce::Colour meterHigh    { 0xffe5484d };

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font mono (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height,
                                          bold ? juce::Font::bold : juce::Font::plain));
}
} // namespace b4nd::theme
