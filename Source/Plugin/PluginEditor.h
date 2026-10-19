// Radio Music (unofficial) - user interface (milestone 2).
// An unofficial, clean-room plugin inspired by Music Thing Modular's Radio Music mk2,
// designed by Tom Whitwell with Chris Johnson of Plus Minus Audio.
#pragma once

#include "PluginProcessor.h"
#include <map>

namespace rmui
{
//==============================================================================
// Rotary knob that draws itself: arc, value in the middle, caption underneath.
class Knob : public juce::Slider
{
public:
    Knob (const juce::String& captionText, std::function<juce::String (double)> formatter);
    void paint (juce::Graphics&) override;

    void setCaption (const juce::String& t) { caption = t; repaint(); }
    void setFormatter (std::function<juce::String (double)> f) { format = std::move (f); repaint(); }

    // When on, dragging the knob moves in whole steps (used for semitones).
    void setSnapToWholeNumbers (bool on) { snapWhole = on; }
    double snapValue (double attempted, DragMode) override { return snapWhole ? std::round (attempted) : attempted; }

private:
    bool snapWhole = false;
    juce::String caption;
    std::function<juce::String (double)> format;
};

//==============================================================================
// Toggle pill, or a momentary round / flat button, drawn by hand.
class RmButton : public juce::Button
{
public:
    enum class Style { Pill, Round, Flat };

    RmButton (const juce::String& label, Style s);
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    void flash() noexcept { flashFrames = 6; repaint(); }
    void tick();

private:
    Style style;
    int flashFrames = 0;
};

//==============================================================================
// Three-segment choice control (Off / Forward / Ping-pong), attached to a choice parameter.
class Segmented : public juce::Component
{
public:
    Segmented (juce::RangedAudioParameter& param, juce::StringArray labels);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::StringArray names;
    juce::ParameterAttachment attachment;
    int selected = 0;
};

//==============================================================================
// The tuning dial: station scale, needle, station name, and the file position / loop window bar.
class DialDisplay : public juce::Component
{
public:
    explicit DialDisplay (RadioMusicProcessor& p);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::Rectangle<float> scaleArea() const;
    void dragTo (float x);

    RadioMusicProcessor& processor;
    juce::ParameterAttachment attachment;
    float stationValue = 0.0f;
    bool dragging = false;
};
} // namespace rmui

//==============================================================================
// The whole interface, laid out on a fixed 940 x 560 canvas. The editor below scales it to the window.
class RadioMusicPanel : public juce::Component,
                        private juce::Timer
{
public:
    explicit RadioMusicPanel (RadioMusicProcessor&);
    ~RadioMusicPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void chooseFolder();
    void addHint (juce::Component& c, const juce::String& text);
    void addHint (juce::Component& c, std::function<juce::String()> text);
    void applySpeedMode (int mode);
    void updateSnap();

    RadioMusicProcessor& processor;

    rmui::DialDisplay dial;
    rmui::Knob startKnob, endKnob, crossfadeKnob, volumeKnob, speedKnob;
    rmui::RmButton resetButton, pauseButton, hostSyncButton, radioStartButton, immediateButton, folderButton;
    rmui::RmButton quantiseButton, gritButton;
    rmui::Segmented loopSelector, speedModeSelector;

    juce::AudioProcessorValueTreeState::SliderAttachment startAttach, endAttach, crossfadeAttach, volumeAttach;
    juce::AudioProcessorValueTreeState::ButtonAttachment pauseAttach, hostSyncAttach, radioStartAttach, immediateAttach,
                                                         quantiseAttach, gritAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;   // follows the speed mode
    juce::ParameterAttachment speedModeWatcher;
    int speedMode = 0;

    std::unique_ptr<juce::FileChooser> chooser;
    std::map<juce::Component*, std::function<juce::String()>> hints;
    juce::String currentHint;
    float meterLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RadioMusicPanel)
};

//==============================================================================
// The plug-in window: resizable, keeps the panel's proportions, remembers its size with the project.
class RadioMusicEditor : public juce::AudioProcessorEditor
{
public:
    static constexpr int kBaseWidth = 940, kBaseHeight = 560;
    static constexpr int kMinWidth = 705, kMaxWidth = 1880;

    explicit RadioMusicEditor (RadioMusicProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    RadioMusicProcessor& processor;
    RadioMusicPanel panel;
    juce::ComponentBoundsConstrainer constrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RadioMusicEditor)
};
