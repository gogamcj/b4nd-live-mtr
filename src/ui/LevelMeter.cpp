#include "ui/LevelMeter.h"
#include "ui/Theme.h"

#include <cmath>

namespace b4nd
{

float LevelMeter::toMeterProportion (float linear) noexcept
{
    if (linear <= 0.0f)
        return 0.0f;
    const float db = juce::jmax (-60.0f, 20.0f * std::log10 (linear));
    return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
}

void LevelMeter::update()
{
    const float v = provider ? provider() : 0.0f;
    const float prev = level;
    level = juce::jmax (v, level * 0.85f);     // fast attack, ~30 Hz release
    if (level < 1.0e-4f) level = 0.0f;

    if (level > 0.0f && level >= hold) { hold = level; holdFrames = 45; }
    else if (--holdFrames <= 0) hold = juce::jmax (0.0f, hold * 0.9f);

    if (v >= 1.0f) clipped = true;

    if (std::abs (prev - level) > 1.0e-6f || hold > 0.0f)
        repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::panelInset);
    g.fillRoundedRectangle (r, 2.0f);

    auto inner = r.reduced (1.0f);
    const float clipH = 4.0f;
    auto clipBox = inner.removeFromTop (clipH);
    g.setColour (clipped ? theme::meterHigh : theme::outline);
    g.fillRect (clipBox);
    inner.removeFromTop (1.0f);

    const float p = toMeterProportion (level);
    if (p > 0.0f)
    {
        auto bar = inner.withTop (inner.getBottom() - inner.getHeight() * p);
        juce::ColourGradient grad (theme::meterLow, 0.0f, inner.getBottom(), theme::meterHigh, 0.0f, inner.getY(), false);
        grad.addColour (0.75, theme::meterMid);
        g.setGradientFill (grad);
        g.fillRect (bar);
    }

    const float hp = toMeterProportion (hold);
    if (hp > 0.0f)
    {
        g.setColour (theme::text);
        g.fillRect (inner.getX(), inner.getBottom() - inner.getHeight() * hp, inner.getWidth(), 1.5f);
    }

    // 0 dB tick
    g.setColour (theme::textFaint);
    const float zeroY = inner.getBottom() - inner.getHeight() * (60.0f / 66.0f);
    g.drawHorizontalLine ((int) zeroY, inner.getX(), inner.getRight());
}

} // namespace b4nd
