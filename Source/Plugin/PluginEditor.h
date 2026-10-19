#pragma once

#include "PluginProcessor.h"

class RadioMusicEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit RadioMusicEditor (RadioMusicProcessor&);
    ~RadioMusicEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void chooseFolder();
    void refreshStatus();

    RadioMusicProcessor& processor;

    juce::TextButton chooseButton { "Choose folder..." };
    juce::Label folderLabel, stationLabel, statusLabel, aboutLabel;
    juce::Label stationCaption, crossfadeCaption, volumeCaption;
    juce::Slider stationSlider, crossfadeSlider, volumeSlider;

    juce::AudioProcessorValueTreeState::SliderAttachment stationAttach, crossfadeAttach, volumeAttach;

    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<const rm::Library> shownLibrary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RadioMusicEditor)
};
