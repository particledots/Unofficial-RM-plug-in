#include "PluginEditor.h"

namespace rmui
{
namespace colours
{
    const juce::Colour bg        { 0xff14120e };
    const juce::Colour panel     { 0xff1e1a15 };
    const juce::Colour panelEdge { 0xff332b21 };
    const juce::Colour glassTop  { 0xff2c2217 };
    const juce::Colour glassBot  { 0xff17110a };
    const juce::Colour amber     { 0xffffb347 };
    const juce::Colour amberDim  { 0xff8f5f1f };
    const juce::Colour cream     { 0xffeee3cf };
    const juce::Colour dim       { 0xff8f8470 };
    const juce::Colour knobBody  { 0xff2a251e };
    const juce::Colour knobRim   { 0xff3d352a };
}

static juce::Font uiFont (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

static juce::Font monoFont (float size)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain));
}

static juce::String formatTime (double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const int total = (int) seconds;
    const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0)
        return juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s).paddedLeft ('0', 2);
    return juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
}

// Shortens text from the left ("...folder/name") so the end of a path stays visible.
static juce::String fitTail (const juce::String& text, const juce::Font& font, float maxWidth)
{
    auto width = [&] (const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, s, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    };

    if (width (text) <= maxWidth)
        return text;

    juce::String s = text;
    while (s.length() > 4 && width (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) + s) > maxWidth)
        s = s.substring (1);

    return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) + s;
}

//==============================================================================
Knob::Knob (const juce::String& captionText, std::function<juce::String (double)> formatter)
    : caption (captionText), format (std::move (formatter))
{
    setSliderStyle (juce::Slider::RotaryVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setMouseDragSensitivity (220);
}

void Knob::paint (juce::Graphics& g)
{
    using namespace colours;
    g.beginTransparencyLayer (isEnabled() ? 1.0f : 0.35f);
    auto b = getLocalBounds().toFloat();
    auto textArea = b.removeFromBottom (32.0f);
    auto valueArea = textArea.removeFromTop (16.0f);
    auto captionArea = textArea;
    const float d = std::min (b.getWidth(), b.getHeight()) - 6.0f;
    const auto c = b.getCentre();
    const juce::Rectangle<float> face (c.x - d * 0.5f, c.y - d * 0.5f, d, d);

    const float start = juce::degreesToRadians (-135.0f);
    const float end   = juce::degreesToRadians (135.0f);
    const float prop  = (float) valueToProportionOfLength (getValue());
    const float angle = start + prop * (end - start);

    // value ring
    const float ringR = d * 0.5f;
    juce::Path track, value;
    track.addCentredArc (c.x, c.y, ringR, ringR, 0.0f, start, end, true);
    value.addCentredArc (c.x, c.y, ringR, ringR, 0.0f, start, angle, true);
    g.setColour (panelEdge);
    g.strokePath (track, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (prop > 0.001f)
    {
        g.setColour (amber.withAlpha (0.25f));
        g.strokePath (value, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (amber);
        g.strokePath (value, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // knob body
    const auto body = face.reduced (9.0f);
    g.setGradientFill (juce::ColourGradient (knobRim, body.getX(), body.getY(), knobBody.darker (0.4f), body.getRight(), body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (knobRim.brighter (0.2f));
    g.drawEllipse (body, 1.0f);

    // pointer
    const float r1 = body.getWidth() * 0.5f - 3.0f, r0 = r1 * 0.38f;
    const float sx = std::sin (angle), sy = -std::cos (angle);
    g.setColour (cream);
    g.drawLine (c.x + sx * r0, c.y + sy * r0, c.x + sx * r1, c.y + sy * r1, 2.5f);

    // value under the knob, caption under the value
    g.setColour (cream);
    g.setFont (monoFont (11.5f));
    g.drawText (format (getValue()), valueArea, juce::Justification::centred, false);

    g.setColour (dim);
    g.setFont (uiFont (10.5f, true));
    g.drawText (caption, captionArea, juce::Justification::centred, false);

    if (hasKeyboardFocus (false))
    {
        g.setColour (amber.withAlpha (0.5f));
        g.drawEllipse (face.expanded (2.0f), 1.0f);
    }

    g.endTransparencyLayer();
}

//==============================================================================
RmButton::RmButton (const juce::String& label, Style s) : juce::Button (label), style (s)
{
    if (style == Style::Pill)
        setClickingTogglesState (true);
}

void RmButton::tick()
{
    if (flashFrames > 0)
    {
        --flashFrames;
        repaint();
    }
}

void RmButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    using namespace colours;
    auto b = getLocalBounds().toFloat().reduced (1.0f);

    if (style == Style::Round)
    {
        const float d = std::min (b.getWidth(), b.getHeight());
        const auto face = juce::Rectangle<float> (d, d).withCentre (b.getCentre());
        const bool lit = down || flashFrames > 0;
        if (lit)
        {
            g.setColour (amber.withAlpha (0.3f));
            g.fillEllipse (face.expanded (5.0f));
        }
        g.setGradientFill (juce::ColourGradient (lit ? amber : knobRim.brighter (over ? 0.15f : 0.0f), face.getX(), face.getY(),
                                                 lit ? amberDim : knobBody.darker (0.3f), face.getRight(), face.getBottom(), false));
        g.fillEllipse (face);
        g.setColour (lit ? amber.brighter (0.3f) : panelEdge.brighter (0.4f));
        g.drawEllipse (face, 1.5f);
        g.setColour (lit ? juce::Colours::black.withAlpha (0.8f) : cream);
        g.setFont (uiFont (11.0f, true));
        g.drawText (getButtonText(), face, juce::Justification::centred, false);
        return;
    }

    const bool on = getToggleState();
    g.beginTransparencyLayer (isEnabled() ? 1.0f : 0.4f);
    const float radius = style == Style::Pill ? b.getHeight() * 0.5f : 6.0f;
    g.setColour (on ? amber.withAlpha (0.14f) : panel);
    g.fillRoundedRectangle (b, radius);
    g.setColour (on ? amber.withAlpha (0.8f) : (over ? dim : panelEdge));
    g.drawRoundedRectangle (b, radius, 1.2f);

    float textLeft = b.getX();
    if (style == Style::Pill)
    {
        const float led = 7.0f;
        const juce::Rectangle<float> ledRect (b.getX() + 9.0f, b.getCentreY() - led * 0.5f, led, led);
        if (on)
        {
            g.setColour (amber.withAlpha (0.35f));
            g.fillEllipse (ledRect.expanded (3.0f));
        }
        g.setColour (on ? amber : panelEdge.brighter (0.3f));
        g.fillEllipse (ledRect);
        textLeft = ledRect.getRight() + 6.0f;
    }

    g.setColour (on ? cream : dim);
    g.setFont (uiFont (9.5f, true));
    g.drawText (getButtonText(), juce::Rectangle<float> (textLeft, b.getY(), b.getRight() - textLeft - 4.0f, b.getHeight()),
                style == Style::Pill ? juce::Justification::centredLeft : juce::Justification::centred, true);
    g.endTransparencyLayer();
}

//==============================================================================
Segmented::Segmented (juce::RangedAudioParameter& param, juce::StringArray labels)
    : names (std::move (labels)),
      attachment (param, [this] (float v) { selected = juce::jlimit (0, names.size() - 1, (int) std::lround (v)); repaint(); })
{
    attachment.sendInitialUpdate();
}

void Segmented::paint (juce::Graphics& g)
{
    using namespace colours;
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (panelEdge);
    g.drawRoundedRectangle (b, 6.0f, 1.2f);

    float totalWeight = 0.0f;
    for (auto& n : names) totalWeight += (float) n.length() + 3.0f;

    float x = b.getX();
    for (int i = 0; i < names.size(); ++i)
    {
        const float w = b.getWidth() * ((float) names[i].length() + 3.0f) / totalWeight;
        auto seg = juce::Rectangle<float> (x, b.getY(), w, b.getHeight()).reduced (2.0f);
        x += w;
        if (i == selected)
        {
            g.setColour (amber);
            g.fillRoundedRectangle (seg, 4.5f);
        }
        g.setColour (i == selected ? juce::Colours::black.withAlpha (0.85f) : dim);
        g.setFont (uiFont (9.5f, true));
        g.drawText (names[i], seg, juce::Justification::centred, false);
    }
}

void Segmented::mouseDown (const juce::MouseEvent& e)
{
    float totalWeight = 0.0f;
    for (auto& n : names) totalWeight += (float) n.length() + 3.0f;

    const float px = e.position.x - 1.0f;
    const float inner = (float) getWidth() - 2.0f;
    float x = 0.0f;
    for (int i = 0; i < names.size(); ++i)
    {
        x += inner * ((float) names[i].length() + 3.0f) / totalWeight;
        if (px < x || i == names.size() - 1)
        {
            attachment.setValueAsCompleteGesture ((float) i);
            return;
        }
    }
}

//==============================================================================
DialDisplay::DialDisplay (RadioMusicProcessor& p)
    : processor (p),
      attachment (*p.apvts.getParameter ("station"), [this] (float v) { stationValue = v; repaint(); })
{
    attachment.sendInitialUpdate();
}

juce::Rectangle<float> DialDisplay::scaleArea() const
{
    return juce::Rectangle<float> (28.0f, 74.0f, (float) getWidth() - 56.0f, 64.0f);
}

void DialDisplay::dragTo (float x)
{
    const auto s = scaleArea();
    const float v = juce::jlimit (0.0f, 1.0f, (x - s.getX()) / s.getWidth());
    attachment.setValueAsPartOfGesture (v);
}

void DialDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (e.position.y < 60.0f || e.position.y > 160.0f)
        return;
    dragging = true;
    attachment.beginGesture();
    dragTo (e.position.x);
}

void DialDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        dragTo (e.position.x);
}

void DialDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        attachment.endGesture();
    }
}

void DialDisplay::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const int count = processor.getEngine().getNumStations();
    if (count < 1)
        return;

    const float delta = std::fabs (w.deltaY) > 0.0001f ? w.deltaY : w.deltaX;
    if (std::fabs (delta) < 0.0001f)
        return;

    const int idx = juce::jlimit (0, count - 1, (int) std::floor (stationValue * (float) count));
    const int next = juce::jlimit (0, count - 1, idx + (delta > 0.0f ? 1 : -1) * (w.isReversed ? -1 : 1));
    attachment.setValueAsCompleteGesture (((float) next + 0.5f) / (float) count);
}

void DialDisplay::paint (juce::Graphics& g)
{
    using namespace colours;
    const auto& engine = processor.getEngine();
    const auto folder = processor.getSampleFolder();
    const auto lib = processor.getLibrary();
    const int count = engine.getNumStations();
    const int idx = engine.getDisplayedStation();       // position on the dial
    const int fileIdx = engine.getDisplayedFile();      // index of the file that is playing
    const int bankNumber = juce::jlimit (0, rm::kNumBanks - 1, (int) std::lround (processor.apvts.getRawParameterValue ("bank")->load()));
    const bool haveStations = lib != nullptr && ! lib->stations.empty() && count > 0;

    auto bounds = getLocalBounds().toFloat();

    // glass panel
    g.setGradientFill (juce::ColourGradient (glassTop, 0.0f, 0.0f, glassBot, 0.0f, bounds.getHeight(), false));
    g.fillRoundedRectangle (bounds, 14.0f);
    g.setColour (panelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 14.0f, 1.5f);

    // ---- station name ----
    auto top = bounds.reduced (28.0f, 0.0f).withTrimmedTop (14.0f).withHeight (46.0f);
    juce::String name, number;
    if (folder == juce::File())
        name = "Choose a folder of WAV, AIFF or RAW files";
    else if (lib == nullptr)
        name = "Scanning...";
    else if (lib->stations.empty())
        name = "No playable audio files in that folder";
    else if (lib->findBank (bankNumber) == nullptr)
        name = "No bank " + juce::String (bankNumber) + " in that folder";
    else if (count < 1)
        name = "Bank " + juce::String (bankNumber) + " has no playable audio files";
    else if (fileIdx >= 0 && fileIdx < (int) lib->stations.size())
    {
        name = lib->stations[(size_t) fileIdx].file.getFileName();

        for (const auto& sl : lib->slots)             // a subfolder shows as "folder / file"
            if (sl.mode != rm::SlotFile && fileIdx >= sl.firstFile && fileIdx < sl.firstFile + sl.numFiles)
            {
                name = sl.name + "  /  " + name + "  (" + juce::String (fileIdx - sl.firstFile + 1) + "/" + juce::String (sl.numFiles) + ")";
                break;
            }

        number = juce::String (idx + 1).paddedLeft ('0', 2) + " / " + juce::String (count).paddedLeft ('0', 2);
    }

    g.setColour (haveStations ? cream : dim);
    g.setFont (uiFont (haveStations ? 24.0f : 18.0f, haveStations));
    g.drawText (name, top.withTrimmedRight (110.0f), juce::Justification::centredLeft, true);

    if (number.isNotEmpty())
    {
        g.setColour (amber);
        g.setFont (monoFont (20.0f));
        g.drawText (number, top.removeFromRight (108.0f), juce::Justification::centredRight, false);
    }

    // ---- tuning scale ----
    const auto sc = scaleArea();
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (sc.expanded (10.0f, 4.0f), 8.0f);

    const int decoCount = 48;
    const int n = haveStations ? count : decoCount;
    const float spacing = sc.getWidth() / (float) n;
    const int labelEvery = std::max (1, (int) std::ceil (46.0f / spacing));

    for (int i = 0; i < n; ++i)
    {
        const float x = sc.getX() + ((float) i + 0.5f) * spacing;
        const bool selected = haveStations && i == idx;
        const bool major = (i % 5) == 0;
        const float h = selected ? sc.getHeight() * 0.66f : (major ? sc.getHeight() * 0.50f : sc.getHeight() * 0.36f);

        if (spacing > 26.0f)                                   // fine ticks between stations, like a radio dial
            for (int m = 1; m < 5; ++m)
            {
                const float mx = x - spacing * 0.5f + spacing * (float) m / 5.0f;
                g.setColour (dim.withAlpha (0.28f));
                g.drawLine (mx, sc.getY() + 2.0f, mx, sc.getY() + 2.0f + sc.getHeight() * 0.16f, 1.0f);
            }

        if (selected)
        {
            g.setColour (amber.withAlpha (0.28f));
            g.drawLine (x, sc.getY() + 2.0f, x, sc.getY() + 2.0f + h, 6.0f);
        }
        g.setColour (selected ? amber : (haveStations ? dim.withAlpha (major ? 0.9f : 0.55f) : dim.withAlpha (0.25f)));
        g.drawLine (x, sc.getY() + 2.0f, x, sc.getY() + 2.0f + h, selected ? 2.0f : (spacing > 3.0f ? 1.4f : 1.0f));

        if (haveStations && (i % labelEvery) == 0 && (selected || spacing >= 14.0f || major))
        {
            g.setColour (selected ? amber : dim);
            g.setFont (monoFont (10.5f));
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (x - 14.0f, sc.getBottom() - 16.0f, 28.0f, 14.0f),
                        juce::Justification::centred, false);
        }
    }

    // needle (follows the Station parameter continuously)
    {
        const float x = sc.getX() + juce::jlimit (0.0f, 1.0f, stationValue) * sc.getWidth();
        g.setColour (amber.withAlpha (0.18f));
        g.drawLine (x, sc.getY() - 6.0f, x, sc.getBottom() + 4.0f, 9.0f);
        g.setColour (amber.withAlpha (0.9f));
        g.drawLine (x, sc.getY() - 6.0f, x, sc.getBottom() + 4.0f, 2.0f);
        juce::Path tri;
        tri.addTriangle (x - 5.0f, sc.getY() - 12.0f, x + 5.0f, sc.getY() - 12.0f, x, sc.getY() - 4.0f);
        g.setColour (amber);
        g.fillPath (tri);
    }

    // ---- file position / loop window bar ----
    const juce::Rectangle<float> bar (28.0f, bounds.getHeight() - 42.0f, bounds.getWidth() - 56.0f, 14.0f);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (bar, 4.0f);

    const double lo = engine.getWindowLo(), hi = engine.getWindowHi();
    const bool rev = engine.isWindowReversed();
    const auto win = juce::Rectangle<float> (bar.getX() + (float) lo * bar.getWidth(), bar.getY(),
                                             std::max (3.0f, (float) (hi - lo) * bar.getWidth()), bar.getHeight());
    g.setColour (amber.withAlpha (0.22f));
    g.fillRoundedRectangle (win, 3.0f);
    g.setColour (amber.withAlpha (0.65f));
    g.drawRoundedRectangle (win, 3.0f, 1.0f);

    // direction chevrons inside the window
    {
        juce::Path chev;
        const float cy = win.getCentreY();
        const float cx = win.getCentreX();
        const float s = 3.6f;
        if (win.getWidth() > 18.0f)
        {
            if (rev) chev.addTriangle (cx + s, cy - s, cx + s, cy + s, cx - s, cy);
            else     chev.addTriangle (cx - s, cy - s, cx - s, cy + s, cx + s, cy);
            g.setColour (amber.withAlpha (0.7f));
            g.fillPath (chev);
        }
    }

    const double pos = engine.getPlayPosition();
    juce::String posText;
    if (pos >= 0.0)
    {
        const float x = bar.getX() + (float) pos * bar.getWidth();
        g.setColour (cream.withAlpha (0.25f));
        g.drawLine (x, bar.getY() - 3.0f, x, bar.getBottom() + 3.0f, 6.0f);
        g.setColour (cream);
        g.drawLine (x, bar.getY() - 3.0f, x, bar.getBottom() + 3.0f, 2.0f);

        if (lib != nullptr && fileIdx >= 0 && fileIdx < (int) lib->stations.size())
        {
            const auto& info = lib->stations[(size_t) fileIdx].info;
            if (info.sampleRate > 0.0)
                posText = formatTime (pos * (double) info.numFrames / info.sampleRate) + " / "
                          + formatTime ((double) info.numFrames / info.sampleRate);
        }
    }

    g.setColour (dim);
    g.setFont (uiFont (9.5f, true));
    g.drawText ("FILE POSITION AND LOOP WINDOW", juce::Rectangle<float> (bar.getX(), bar.getBottom() + 2.0f, 260.0f, 14.0f),
                juce::Justification::centredLeft, false);
    g.setFont (monoFont (10.0f));
    g.drawText (posText, juce::Rectangle<float> (bar.getRight() - 200.0f, bar.getBottom() + 2.0f, 200.0f, 14.0f),
                juce::Justification::centredRight, false);
}

//==============================================================================
BankStepper::BankStepper (RadioMusicProcessor& p)
    : processor (p),
      attachment (*p.apvts.getParameter ("bank"), [this] (float v) { bank = juce::jlimit (0, rm::kNumBanks - 1, (int) std::lround (v)); repaint(); })
{
    attachment.sendInitialUpdate();
}

void BankStepper::step (int delta)
{
    const int next = juce::jlimit (0, rm::kNumBanks - 1, bank + delta);
    if (next != bank)
        attachment.setValueAsCompleteGesture ((float) next);
}

void BankStepper::mouseDown (const juce::MouseEvent& e)
{
    step (e.x < getWidth() / 2 ? -1 : 1);
}

void BankStepper::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const float delta = std::fabs (w.deltaY) > 0.0001f ? w.deltaY : w.deltaX;
    if (std::fabs (delta) > 0.0001f)
        step ((delta > 0.0f ? 1 : -1) * (w.isReversed ? -1 : 1));
}

void BankStepper::paint (juce::Graphics& g)
{
    using namespace colours;
    const auto lib = processor.getLibrary();
    const bool exists = lib != nullptr && lib->findBank (bank) != nullptr && (lib->hasNumberedBanks || bank == 0);

    auto b = getLocalBounds().toFloat();
    g.setColour (glassBot);
    g.fillRoundedRectangle (b, 8.0f);
    g.setColour (panelEdge);
    g.drawRoundedRectangle (b.reduced (0.5f), 8.0f, 1.2f);

    // arrows
    g.setColour (bank > 0 ? amber : dim.withAlpha (0.4f));
    juce::Path l;
    l.addTriangle (14.0f, b.getCentreY(), 22.0f, b.getCentreY() - 6.0f, 22.0f, b.getCentreY() + 6.0f);
    g.fillPath (l);
    g.setColour (bank < rm::kNumBanks - 1 ? amber : dim.withAlpha (0.4f));
    juce::Path r;
    r.addTriangle (b.getRight() - 14.0f, b.getCentreY(), b.getRight() - 22.0f, b.getCentreY() - 6.0f, b.getRight() - 22.0f, b.getCentreY() + 6.0f);
    g.fillPath (r);

    g.setColour (dim);
    g.setFont (uiFont (9.0f, true));
    g.drawText ("BANK", b.withTrimmedLeft (28.0f).withTrimmedRight (28.0f).withTrimmedBottom (b.getHeight() * 0.55f),
                juce::Justification::centred, false);

    g.setColour (exists ? amber : dim);
    g.setFont (monoFont (20.0f));
    g.drawText (juce::String (bank).paddedLeft ('0', 2), b.withTrimmedLeft (28.0f).withTrimmedRight (28.0f).withTrimmedTop (b.getHeight() * 0.3f),
                juce::Justification::centred, false);
}
} // namespace rmui

//==============================================================================
namespace
{
    juce::String formatCrossfade (double ms)
    {
        if (ms < 10.0)    return juce::String (ms, 1) + " ms";
        if (ms < 1000.0)  return juce::String ((int) std::lround (ms)) + " ms";
        if (ms < 10000.0) return juce::String (ms / 1000.0, 2) + " s";
        return juce::String (ms / 1000.0, 1) + " s";
    }

    juce::String formatVolume (double dB)
    {
        if (dB <= -59.9) return "-inf";
        return juce::String (dB, 1) + " dB";
    }

    juce::String formatPercent (double norm, int divider)
    {
        const int step = rm::quantiseStep ((float) norm, divider);
        return juce::String ((double) step * 100.0 / (double) rm::kStartSteps, 1) + "%";
    }
}

RadioMusicPanel::RadioMusicPanel (RadioMusicProcessor& p)
    : processor (p),
      dial (p),
      bankStepper (p),
      startKnob ("START",     [&p] (double v) { return formatPercent (v, p.getStartDivider()); }),
      endKnob ("END",         [] (double v) { return formatPercent (v, 1); }),
      crossfadeKnob ("CROSSFADE", [] (double v) { return formatCrossfade (v); }),
      volumeKnob ("VOLUME",   [] (double v) { return formatVolume (v); }),
      speedKnob ("SPEED",     [] (double v) { return juce::String (std::lround (v)) + "%"; }),
      noiseKnob ("NOISE",     [] (double v) { return juce::String (std::lround (v)); }),
      whistleKnob ("WHISTLE", [] (double v) { return juce::String (std::lround (v)); }),
      strengthKnob ("STRENGTH", [] (double v) { return juce::String (std::lround (v * 100.0 / 256.0)) + "%"; }),
      advanceKnob ("ADVANCE",  [] (double v) { return juce::String (std::lround (v)) + "%"; }),
      resetButton ("RESET", rmui::RmButton::Style::Round),
      pauseButton ("PAUSE", rmui::RmButton::Style::Pill),
      hostSyncButton ("HOST SYNC", rmui::RmButton::Style::Pill),
      radioStartButton ("RADIO START", rmui::RmButton::Style::Pill),
      immediateButton ("START NOW", rmui::RmButton::Style::Pill),
      folderButton ("CHOOSE FOLDER", rmui::RmButton::Style::Flat),
      rescanButton ("RESCAN", rmui::RmButton::Style::Flat),
      quantiseButton ("QUANTISE", rmui::RmButton::Style::Pill),
      gritButton ("GRIT", rmui::RmButton::Style::Pill),
      ssbButton ("SSB EFFECT", rmui::RmButton::Style::Pill),
      loopSelector (*p.apvts.getParameter ("loopMode"), juce::StringArray { "OFF", "LOOP", "PING-PONG" }),
      speedModeSelector (*p.apvts.getParameter ("speedMode"), juce::StringArray { "TAPE", "NOTES", "'90s" }),
      tunerSelector (*p.apvts.getParameter ("tunerMode"), juce::StringArray { "SHARP", "SOFT", "RADIO" }),
      fadeSelector (*p.apvts.getParameter ("fadeMode"), juce::StringArray { "CONST POWER", "LINEAR" }),
      startAttach     (p.apvts, "start",     startKnob),
      endAttach       (p.apvts, "end",       endKnob),
      crossfadeAttach (p.apvts, "crossfade", crossfadeKnob),
      volumeAttach    (p.apvts, "volume",    volumeKnob),
      noiseAttach     (p.apvts, "noiseVol",      noiseKnob),
      whistleAttach   (p.apvts, "whistleVol",    whistleKnob),
      strengthAttach  (p.apvts, "radioStrength", strengthKnob),
      advanceAttach   (p.apvts, "advance90",     advanceKnob),
      pauseAttach      (p.apvts, "pause",          pauseButton),
      hostSyncAttach   (p.apvts, "hostSync",       hostSyncButton),
      radioStartAttach (p.apvts, "radioStart",     radioStartButton),
      immediateAttach  (p.apvts, "startImmediate", immediateButton),
      quantiseAttach   (p.apvts, "quantisePitch",  quantiseButton),
      gritAttach       (p.apvts, "gritMode",       gritButton),
      ssbAttach        (p.apvts, "ssbEffect",      ssbButton),
      speedModeWatcher (*p.apvts.getParameter ("speedMode"), [this] (float v) { applySpeedMode (juce::jlimit (0, 2, (int) std::lround (v))); }),
      tunerWatcher     (*p.apvts.getParameter ("tunerMode"), [this] (float v) { applyTunerMode (juce::jlimit (0, 2, (int) std::lround (v))); })
{
    setSize (RadioMusicEditor::kBaseWidth, RadioMusicEditor::kBaseHeight);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &dial, &startKnob, &endKnob, &crossfadeKnob, &volumeKnob, &resetButton, &pauseButton,
             &hostSyncButton, &radioStartButton, &immediateButton, &folderButton, &rescanButton, &loopSelector,
             &speedKnob, &speedModeSelector, &quantiseButton, &gritButton, &bankStepper,
             &tunerSelector, &fadeSelector, &noiseKnob, &whistleKnob, &strengthKnob, &ssbButton, &advanceKnob })
        addAndMakeVisible (c);

    startKnob.setDoubleClickReturnValue (true, 0.0);
    endKnob.setDoubleClickReturnValue (true, 1.0);
    crossfadeKnob.setDoubleClickReturnValue (true, 25.0);
    volumeKnob.setDoubleClickReturnValue (true, 0.0);
    noiseKnob.setDoubleClickReturnValue (true, 50.0);
    whistleKnob.setDoubleClickReturnValue (true, 50.0);
    strengthKnob.setDoubleClickReturnValue (true, 128.0);
    advanceKnob.setDoubleClickReturnValue (true, 100.0);

    speedModeWatcher.sendInitialUpdate();
    tunerWatcher.sendInitialUpdate();

    quantiseButton.onStateChange = [this] { updateSnap(); };
    resetButton.onClick  = [this] { processor.triggerReset(); resetButton.flash(); };
    folderButton.onClick = [this] { chooseFolder(); };
    rescanButton.onClick = [this] { processor.rescanFolder(); };

    addHint (dial,             "Drag along the dial (or use the mouse wheel) to tune. Each audio file in the folder is a station, in file-name order.");
    addHint (startKnob,        "Where playback starts, and the left edge of the loop. Takes effect on RESET unless START NOW is on. If START is right of END, the loop plays in reverse.");
    addHint (endKnob,          "The right edge of the loop. START at far left with END at either edge plays the whole file.");
    addHint (resetButton,      "Restart the tuned station from START. A MIDI note-on for note 36 (C1) does the same.");
    addHint (crossfadeKnob, [this]() -> juce::String
    {
        if (tunerMode == 1) return "Not used in SOFT mode: stations blend with the dial instead of by time. Switch to SHARP or RADIO to use it.";
        return "How long it takes to blend from one station to the next when you tune (up to 60 seconds). Double-click to reset to 25 ms.";
    });
    addHint (tunerSelector,    "SHARP: one station at a time, with a crossfade between stations. SOFT: neighbouring stations blend smoothly as you move the dial, so two can play at once. RADIO: stations emerge from hiss, with a whistle as you tune in.");
    addHint (fadeSelector,     "The shape of crossfades and of the SOFT blend. CONST POWER suits almost all audio. LINEAR suits correlated sounds such as wavetables and envelopes.");
    addHint (noiseKnob,        "RADIO mode: how loud the hiss is. It fills the gaps between stations and fades out as a strong station comes in. Double-click for 50.");
    addHint (whistleKnob,      "RADIO mode: the whistle that falls in pitch as you tune onto a station and disappears when you are exactly on it. Double-click for 50.");
    addHint (strengthKnob,     "RADIO mode: how strong the weakest stations are. 100% = every station comes in at full strength. 0% = stations range from almost inaudible to full. Double-click for 50%.");
    addHint (ssbButton,        "RADIO mode: a ring-modulation style frequency shift on stations that are not exactly tuned. It fades away as you tune onto the station.");
    addHint (volumeKnob,       "Output level. Double-click to reset to 0 dB.");
    addHint (loopSelector,     "OFF: stop at the end of the loop until RESET or retuning. LOOP: repeat. PING-PONG: forwards, then backwards.");
    addHint (pauseButton,      "Tape-style stop: the speed slows to zero and the sound fades out, then picks up again when released. In NOTES mode it stops almost at once. In '90s mode the advance freezes but the sound keeps playing.");
    addHint (speedKnob, [this]() -> juce::String
    {
        if (speedMode == 1) return "Pitch in semitones (-48 to +24). Only the tuned station changes pitch; other stations keep running at normal speed. Double-click for 0.";
        if (effectiveSpeedMode() == 2) return "'90s mode: the pitch of the sound, as a percent of original (100% = normal, negative = backwards). How fast the file moves forward is set by ADVANCE. Double-click for 100%.";
        if (speedMode == 2) return "'90s mode is only available with the SHARP tuner, so TAPE speed is used. Switch the tuner to SHARP to use it.";
        return "Playback speed as a percent of original (100% = normal, 0% = stopped, negative plays backwards, up to 400%). Only the tuned station changes speed; other stations keep running at normal speed. Double-click for 100%.";
    });
    addHint (speedModeSelector, "TAPE: smooth speed control, through zero into reverse. NOTES: pitch in semitones. '90s: a lo-fi time stretch where pitch and the rate the file moves forward are separate (SHARP tuner only).");
    addHint (advanceKnob,      "'90s mode: how fast the file moves forward, as a percent of normal (0% freezes it, 100% = normal, up to 400%), independent of the pitch. Pause also freezes it. Double-click for 100%.");
    addHint (quantiseButton,   "Only works in NOTES mode. On: the pitch knob moves in whole semitones (equal temperament). Off: continuous pitch, so you can slide between notes.");
    addHint (gritButton,       "Low-fi sound: 8-bit, about 20 kHz, no smoothing between samples, softened by a post-DAC filter. Off is clean cubic interpolation.");
    addHint (radioStartButton, "ON: every file keeps playing in the background, like tuning a radio. OFF: a station restarts from START when you tune to it.");
    addHint (immediateButton,  "ON: moving START takes effect straight away instead of waiting for RESET.");
    addHint (hostSyncButton,   "ON: the radio clock follows the DAW's play position, so bounces repeat exactly. Silent while the transport is stopped.");
    addHint (folderButton,     "Choose the folder to play. Audio files (WAV, AIFF, RAW) in it are the stations. Folders named 0 to 15 (like \"05 shortwave\") are banks; subfolders pick one file at a time.");
    addHint (rescanButton,     "Read the folder again: picks up new files, subfolders and a new or changed settings.txt (which is applied to the knobs). Stays on the current bank.");
    addHint (bankStepper,      "Bank 0 to 15: root folders whose names start with a number. Click the left or right side, or use the mouse wheel. Without numbered folders, the whole folder is bank 0.");

    addMouseListener (this, true);
    startTimerHz (30);
}

RadioMusicPanel::~RadioMusicPanel()
{
    stopTimer();
}

void RadioMusicPanel::addHint (juce::Component& c, const juce::String& text)
{
    hints[&c] = [text] { return text; };
}

void RadioMusicPanel::addHint (juce::Component& c, std::function<juce::String()> text)
{
    hints[&c] = std::move (text);
}

void RadioMusicPanel::applySpeedMode (int mode)
{
    speedMode = mode;
    refreshSpeedUi();
}

// '90s mode is only available with the Sharp tuner; with Soft or Radio the plug-in uses Tape speed.
int RadioMusicPanel::effectiveSpeedMode() const noexcept
{
    return (speedMode == 2 && tunerMode != 0) ? 0 : speedMode;
}

void RadioMusicPanel::refreshSpeedUi()
{
    const int mode = effectiveSpeedMode();
    speedAttach.reset();

    if (mode == 1)
    {
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "speedNotes", speedKnob);
        speedKnob.setCaption ("PITCH");
        speedKnob.setDoubleClickReturnValue (true, 0.0);
        speedKnob.setFormatter ([this] (double v)
        {
            const bool quant = processor.apvts.getRawParameterValue ("quantisePitch")->load() > 0.5f;
            const double st = quant ? std::round (v) : v;
            return juce::String (st >= 0.0 ? "+" : "") + juce::String (st, quant ? 0 : 1) + " st";
        });
    }
    else
    {
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "speedTape", speedKnob);
        speedKnob.setCaption (mode == 2 ? "PITCH" : "SPEED");
        speedKnob.setDoubleClickReturnValue (true, 100.0);
        speedKnob.setFormatter ([] (double v) { return juce::String (std::lround (v)) + "%"; });
    }

    speedKnob.setEnabled (true);
    quantiseButton.setVisible (mode != 2);
    advanceKnob.setVisible (mode == 2);
    quantiseButton.setEnabled (mode == 1);
    updateSnap();
    repaint();
}

void RadioMusicPanel::applyTunerMode (int mode)
{
    tunerMode = mode;
    refreshSpeedUi();
    const bool radio = mode == 2;

    for (juce::Component* c : std::initializer_list<juce::Component*> { &noiseKnob, &whistleKnob, &strengthKnob, &ssbButton })
        c->setEnabled (radio);

    crossfadeKnob.setEnabled (mode != 1);
    repaint();
}

void RadioMusicPanel::updateSnap()
{
    speedKnob.setSnapToWholeNumbers (speedMode == 1 && quantiseButton.getToggleState());
}

void RadioMusicPanel::mouseEnter (const juce::MouseEvent& e)
{
    auto it = hints.find (e.eventComponent);
    if (it != hints.end())
    {
        const auto text = it->second();
        if (currentHint != text)
        {
            currentHint = text;
            repaint (getLocalBounds().removeFromBottom (100));
        }
    }
}

void RadioMusicPanel::mouseExit (const juce::MouseEvent& e)
{
    auto it = hints.find (e.eventComponent);
    if (it != hints.end() && currentHint == it->second())
    {
        currentHint = {};
        repaint (getLocalBounds().removeFromBottom (100));
    }
}

void RadioMusicPanel::resized()
{
    using juce::Rectangle;

    dial.setBounds (24, 70, getWidth() - 48, 212);
    bankStepper.setBounds (330, 16, 118, 40);

    // controls panel: y 294..464
    const int ky = 344, kw = 88, kh = 112;
    startKnob.setBounds     (Rectangle<int> (44, ky, kw, kh));
    resetButton.setBounds   (Rectangle<int> (146, ky + 12, 56, 56));
    endKnob.setBounds       (Rectangle<int> (212, ky, kw, kh));

    speedModeSelector.setBounds (Rectangle<int> (326, 314, 206, 24));
    speedKnob.setBounds     (Rectangle<int> (332, ky, kw, kh));
    quantiseButton.setBounds (Rectangle<int> (436, ky + 28, 96, 24));
    advanceKnob.setBounds   (Rectangle<int> (436, ky, kw, kh));

    crossfadeKnob.setBounds (Rectangle<int> (556, ky, kw, kh));
    volumeKnob.setBounds    (Rectangle<int> (644, ky, kw, kh));

    const int rx = 754, rw = 146;
    loopSelector.setBounds     (Rectangle<int> (rx, 318, rw, 26));
    pauseButton.setBounds      (Rectangle<int> (rx, 350, 62, 24));
    hostSyncButton.setBounds   (Rectangle<int> (rx + 66, 350, 80, 24));
    radioStartButton.setBounds (Rectangle<int> (rx, 378, rw, 24));
    immediateButton.setBounds  (Rectangle<int> (rx, 406, rw, 24));
    gritButton.setBounds       (Rectangle<int> (rx, 434, rw, 24));

    // tuner panel: y 470..584
    tunerSelector.setBounds (Rectangle<int> (44, 494, 206, 26));
    fadeSelector.setBounds  (Rectangle<int> (44, 540, 206, 26));
    noiseKnob.setBounds     (Rectangle<int> (332, 488, 88, 96));
    whistleKnob.setBounds   (Rectangle<int> (424, 488, 88, 96));
    strengthKnob.setBounds  (Rectangle<int> (516, 488, 88, 96));
    ssbButton.setBounds     (Rectangle<int> (620, 520, 120, 24));

    folderButton.setBounds (Rectangle<int> (24, 638, 120, 26));
    rescanButton.setBounds (Rectangle<int> (150, 638, 70, 26));
}

void RadioMusicPanel::paint (juce::Graphics& g)
{
    using namespace rmui::colours;
    g.fillAll (bg);

    // header
    g.setColour (cream);
    g.setFont (rmui::uiFont (26.0f, true));
    g.drawText ("RADIO MUSIC", juce::Rectangle<int> (26, 14, 240, 36), juce::Justification::centredLeft, false);
    g.setColour (amber);
    g.setFont (rmui::uiFont (12.0f, true));
    g.drawText ("UNOFFICIAL", juce::Rectangle<int> (200, 20, 120, 24), juce::Justification::centredLeft, false);

    // radio clock
    {
        auto box = juce::Rectangle<float> ((float) getWidth() - 24.0f - 216.0f, 14.0f, 216.0f, 40.0f);
        g.setColour (glassBot);
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.2f);

        const double s = processor.getEngine().getClockSeconds();
        const int total = (int) s;
        const juce::String t = juce::String (total / 3600) + ":" + juce::String ((total / 60) % 60).paddedLeft ('0', 2)
                               + ":" + juce::String (total % 60).paddedLeft ('0', 2) + "." + juce::String ((int) ((s - (double) total) * 10.0));
        g.setColour (amber);
        g.setFont (rmui::monoFont (20.0f));
        g.drawText (t, box.withTrimmedLeft (12.0f).withTrimmedRight (60.0f), juce::Justification::centredLeft, false);
        g.setColour (dim);
        g.setFont (rmui::uiFont (9.0f, true));
        g.drawText ("RADIO CLOCK", box.removeFromRight (82.0f).withTrimmedRight (8.0f), juce::Justification::centredRight, false);
    }

    // output meter
    {
        const auto m = juce::Rectangle<float> ((float) getWidth() - 24.0f - 216.0f - 40.0f - 200.0f, 28.0f, 200.0f, 10.0f);
        g.setColour (glassBot);
        g.fillRoundedRectangle (m, 5.0f);
        const float dB = meterLevel > 0.00001f ? juce::Decibels::gainToDecibels (meterLevel) : -90.0f;
        const float prop = juce::jlimit (0.0f, 1.0f, (dB + 60.0f) / 60.0f);
        auto fill = m.withWidth (std::max (0.0f, m.getWidth() * prop));
        g.setColour (dB > -3.0f ? juce::Colour (0xffff6b4a) : amber);
        g.fillRoundedRectangle (fill, 5.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (m, 5.0f, 1.0f);
        g.setColour (dim);
        g.setFont (rmui::uiFont (9.0f, true));
        g.drawText ("OUTPUT", juce::Rectangle<float> (m.getX(), m.getBottom() + 3.0f, 100.0f, 12.0f), juce::Justification::centredLeft, false);
    }

    // controls panel
    {
        const auto panelRect = juce::Rectangle<float> (24.0f, 294.0f, (float) getWidth() - 48.0f, 170.0f);
        g.setColour (panel);
        g.fillRoundedRectangle (panelRect, 14.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (panelRect.reduced (0.5f), 14.0f, 1.5f);

        g.setColour (panelEdge);
        for (float x : { 316.0f, 546.0f, 742.0f })
            g.drawLine (x, 312.0f, x, 450.0f, 1.0f);

        g.setColour (dim);
        g.setFont (rmui::uiFont (9.5f, true));
        g.drawText ("LOOP WINDOW", juce::Rectangle<int> (44, 298, 260, 14), juce::Justification::centredLeft, false);
        g.drawText ("SPEED", juce::Rectangle<int> (326, 298, 200, 14), juce::Justification::centredLeft, false);
        g.drawText ("TUNING AND LEVEL", juce::Rectangle<int> (556, 298, 180, 14), juce::Justification::centredLeft, false);
        g.drawText ("PLAYBACK", juce::Rectangle<int> (754, 298, 140, 14), juce::Justification::centredLeft, false);
    }

    // tuner panel
    {
        const auto panelRect = juce::Rectangle<float> (24.0f, 470.0f, (float) getWidth() - 48.0f, 114.0f);
        g.setColour (panel);
        g.fillRoundedRectangle (panelRect, 14.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (panelRect.reduced (0.5f), 14.0f, 1.5f);
        g.drawLine (316.0f, 488.0f, 316.0f, 566.0f, 1.0f);
        g.drawLine (750.0f, 488.0f, 750.0f, 566.0f, 1.0f);

        g.setColour (dim);
        g.setFont (rmui::uiFont (9.5f, true));
        g.drawText ("TUNER MODE", juce::Rectangle<int> (44, 476, 200, 14), juce::Justification::centredLeft, false);
        g.drawText ("FADE CURVE", juce::Rectangle<int> (44, 524, 200, 14), juce::Justification::centredLeft, false);
        g.drawText ("RADIO MODE", juce::Rectangle<int> (326, 476, 200, 14), juce::Justification::centredLeft, false);

        g.setColour (dim.withAlpha (0.8f));
        g.setFont (rmui::uiFont (10.5f));
        g.drawFittedText ("Noise, whistle, strength and SSB only act in RADIO mode. In SOFT mode the crossfade time is not used.",
                          juce::Rectangle<int> (762, 492, 140, 70), juce::Justification::topLeft, 5);
    }

    // hint line
    g.setColour (currentHint.isEmpty() ? dim.withAlpha (0.55f) : cream.withAlpha (0.9f));
    g.setFont (rmui::uiFont (14.5f));
    g.drawFittedText (currentHint.isEmpty() ? juce::String ("Hover over a control to see what it does.") : currentHint,
                      juce::Rectangle<int> (28, 590, getWidth() - 56, 44), juce::Justification::centredLeft, 2, 0.9f);

    // folder path
    {
        const auto folder = processor.getSampleFolder();
        const juce::String text = folder == juce::File() ? juce::String ("No folder chosen") : folder.getFullPathName();
        const auto font = rmui::uiFont (11.0f);
        g.setColour (dim);
        g.setFont (font);
        g.drawText (rmui::fitTail (text, font, 270.0f), juce::Rectangle<int> (230, 634, 280, 18), juce::Justification::centredLeft, false);

        // what the plug-in found in the folder
        juce::String info;
        const auto lib = processor.getLibrary();
        if (lib != nullptr && folder != juce::File())
        {
            const int bankNumber = juce::jlimit (0, rm::kNumBanks - 1, (int) std::lround (processor.apvts.getRawParameterValue ("bank")->load()));
            if (const auto* b = lib->findBank (bankNumber))
            {
                int files = 0;
                for (int i = b->firstSlot; i < b->firstSlot + b->numSlots; ++i)
                    files += lib->slots[(size_t) i].numFiles;

                info = "Bank " + juce::String (bankNumber) + ": " + juce::String (b->numSlots) + " stations, " + juce::String (files) + " files. ";
                if (lib->rootSettingsFound || b->ownSettingsFound)
                    info += "settings.txt: " + juce::String ((int) b->settings.size()) + " keys.";
                else
                    info += "No settings.txt.";

                const auto& eng = processor.getEngine();
                info += "\nResets: " + juce::String ((int) eng.getResetsApplied()) + " done, " + juce::String ((int) eng.getResetsIgnored())
                        + " skipped, " + juce::String ((int) eng.getFileChanges()) + " file changes";
            }
            else
                info = "No bank " + juce::String (bankNumber) + " in this folder.  " + juce::String ((int) lib->banks.size()) + " banks found.";
        }

        g.setFont (rmui::uiFont (10.0f));
        g.setColour (rmui::colours::dim.withAlpha (0.8f));
        g.drawFittedText (info, juce::Rectangle<int> (230, 652, 280, 26), juce::Justification::topLeft, 2);
    }

    // credits
    g.setColour (dim.withAlpha (0.85f));
    g.setFont (rmui::uiFont (9.0f));
    g.drawFittedText ("Unofficial plug-in inspired by Radio Music mk2 by Music Thing Modular, designed by Tom Whitwell with Chris Johnson "
                      "of Plus Minus Audio. Not affiliated with or endorsed by Music Thing Modular. Open source (AGPLv3); clean-room "
                      "implementation from the published documentation. Buffer underruns: " + juce::String ((int) processor.getEngine().getUnderrunCount()),
                      juce::Rectangle<int> (520, 632, getWidth() - 24 - 520, 40), juce::Justification::centredRight, 4);
}

void RadioMusicPanel::chooseFolder()
{
    const auto start = processor.getSampleFolder() != juce::File()
                           ? processor.getSampleFolder()
                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose the folder of audio files (WAV or AIFF)", start);

    juce::Component::SafePointer<RadioMusicPanel> safe (this);

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;

                              const auto result = fc.getResult();
                              if (result.isDirectory())
                                  safe->processor.setSampleFolder (result);
                          });
}

void RadioMusicPanel::timerCallback()
{
    const float peak = processor.getOutputPeak();
    meterLevel = std::max (peak, meterLevel * 0.86f);

    resetButton.tick();
    dial.repaint();
    repaint (juce::Rectangle<int> (0, 0, getWidth(), 66));
    repaint (juce::Rectangle<int> (0, 630, getWidth(), 54));
}


//==============================================================================
RadioMusicEditor::RadioMusicEditor (RadioMusicProcessor& p)
    : AudioProcessorEditor (&p), processor (p), panel (p)
{
    addAndMakeVisible (panel);

    constrainer.setFixedAspectRatio ((double) kBaseWidth / (double) kBaseHeight);
    constrainer.setSizeLimits (kMinWidth, kMinWidth * kBaseHeight / kBaseWidth, kMaxWidth, kMaxWidth * kBaseHeight / kBaseWidth);
    setConstrainer (&constrainer);
    setResizable (true, true);

    const int w = juce::jlimit (kMinWidth, kMaxWidth, (int) processor.apvts.state.getProperty ("editorWidth", kBaseWidth));
    setSize (w, (int) std::lround ((double) w * kBaseHeight / kBaseWidth));
}

void RadioMusicEditor::paint (juce::Graphics& g)
{
    g.fillAll (rmui::colours::bg);
}

void RadioMusicEditor::resized()
{
    panel.setBounds (0, 0, kBaseWidth, kBaseHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) kBaseWidth));

    if ((int) processor.apvts.state.getProperty ("editorWidth", kBaseWidth) != getWidth())
        processor.apvts.state.setProperty ("editorWidth", getWidth(), nullptr);
}
