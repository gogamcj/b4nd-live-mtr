#include "ui/B4ndLookAndFeel.h"
#include "ui/Theme.h"

namespace b4nd
{

B4ndLookAndFeel::B4ndLookAndFeel()
{
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour (ColourScheme::windowBackground, theme::background);
    scheme.setUIColour (ColourScheme::widgetBackground, theme::panelRaised);
    scheme.setUIColour (ColourScheme::menuBackground, theme::panel);
    scheme.setUIColour (ColourScheme::outline, theme::outline);
    scheme.setUIColour (ColourScheme::defaultText, theme::text);
    scheme.setUIColour (ColourScheme::defaultFill, theme::accent);
    scheme.setUIColour (ColourScheme::highlightedText, theme::text);
    scheme.setUIColour (ColourScheme::highlightedFill, theme::accent.withAlpha (0.35f));
    scheme.setUIColour (ColourScheme::menuText, theme::text);
    setColourScheme (scheme);

    setColour (juce::TextButton::buttonColourId, theme::panelRaised);
    setColour (juce::TextButton::buttonOnColourId, theme::accent);
    setColour (juce::TextButton::textColourOffId, theme::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::Label::textColourId, theme::text);
    setColour (juce::ListBox::backgroundColourId, theme::panel);
    setColour (juce::ListBox::outlineColourId, theme::outline);
    setColour (juce::Slider::thumbColourId, theme::text);
    setColour (juce::Slider::trackColourId, theme::accent);
    setColour (juce::Slider::backgroundColourId, theme::panelInset);
    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::panelInset);
    setColour (juce::Slider::textBoxTextColourId, theme::textDim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ResizableWindow::backgroundColourId, theme::background);
    setColour (juce::DocumentWindow::textColourId, theme::text);
    setColour (juce::TooltipWindow::backgroundColourId, theme::panelRaised);
    setColour (juce::TooltipWindow::textColourId, theme::text);
}

void B4ndLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                            bool isMouseOverButton, bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const float corner = juce::jmin (6.0f, bounds.getHeight() * 0.2f);

    auto base = backgroundColour;
    if (! button.isEnabled())
        base = base.withMultipliedAlpha (0.45f);
    else if (isButtonDown)
        base = base.brighter (0.25f);
    else if (isMouseOverButton)
        base = base.brighter (0.1f);

    g.setColour (base);
    g.fillRoundedRectangle (bounds, corner);
    g.setColour (button.getToggleState() ? base.brighter (0.4f) : theme::outline);
    g.drawRoundedRectangle (bounds, corner, 1.0f);
}

juce::Font B4ndLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    // Fit the longest line of the label to the button width as well as its height.
    int longest = 1;
    for (auto& line : juce::StringArray::fromLines (button.getButtonText()))
        longest = juce::jmax (longest, line.length());
    const float byWidth = (float) button.getWidth() / ((float) longest * 0.68f + 1.2f);
    return theme::font (juce::jlimit (10.0f, 30.0f, juce::jmin (buttonHeight * 0.42f, byWidth)), true);
}

void B4ndLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float, float, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, slider);
        return;
    }

    // Fader: inset slot + rectangular cap.
    const float cx = x + width * 0.5f;
    juce::Rectangle<float> slot (cx - 2.0f, (float) y, 4.0f, (float) height);
    g.setColour (theme::panelInset);
    g.fillRoundedRectangle (slot, 2.0f);
    g.setColour (slider.findColour (juce::Slider::trackColourId).withAlpha (0.7f));
    g.fillRoundedRectangle (slot.withTop (sliderPos), 2.0f);

    juce::Rectangle<float> cap (cx - 11.0f, sliderPos - 6.0f, 22.0f, 12.0f);
    g.setColour (theme::text.withAlpha (slider.isEnabled() ? 0.9f : 0.4f));
    g.fillRoundedRectangle (cap, 2.0f);
    g.setColour (theme::panelInset);
    g.drawHorizontalLine ((int) sliderPos, cap.getX() + 3.0f, cap.getRight() - 3.0f);
}

void B4ndLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float centreAngle = (startAngle + endAngle) * 0.5f;

    g.setColour (theme::panelInset);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f,
                       juce::jmin (centreAngle, angle), juce::jmax (centreAngle, angle), true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
    g.strokePath (arc, juce::PathStrokeType (2.5f));

    juce::Path needle;
    needle.addRectangle (-1.0f, -radius + 3.0f, 2.0f, radius * 0.55f);
    g.setColour (theme::text);
    g.fillPath (needle, juce::AffineTransform::rotation (angle).translated (centre));
}

void B4ndLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                                     bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                                     bool isMouseOver, bool)
{
    juce::Rectangle<int> thumb = isScrollbarVertical ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize)
                                                     : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height);
    g.setColour (theme::textFaint.withAlpha (isMouseOver ? 0.8f : 0.5f));
    g.fillRoundedRectangle (thumb.reduced (2).toFloat(), 3.0f);
}

} // namespace b4nd
