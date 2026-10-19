#include "PluginEditor.h"

RadioMusicEditor::RadioMusicEditor (RadioMusicProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      stationAttach   (p.apvts, "station",   stationSlider),
      crossfadeAttach (p.apvts, "crossfade", crossfadeSlider),
      volumeAttach    (p.apvts, "volume",    volumeSlider)
{
    setSize (520, 330);

    addAndMakeVisible (chooseButton);
    chooseButton.onClick = [this] { chooseFolder(); };

    for (auto* l : { &folderLabel, &stationLabel, &statusLabel, &aboutLabel })
    {
        addAndMakeVisible (l);
        l->setJustificationType (juce::Justification::centredLeft);
    }

    folderLabel.setFont (juce::FontOptions (13.0f));
    stationLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    statusLabel.setFont (juce::FontOptions (12.0f));
    aboutLabel.setFont (juce::FontOptions (11.0f));
    aboutLabel.setJustificationType (juce::Justification::topLeft);
    aboutLabel.setText ("Unofficial plugin inspired by Radio Music mk2 by Music Thing Modular "
                        "(designed by Tom Whitwell with Chris Johnson of Plus Minus Audio). "
                        "Not affiliated with or endorsed by Music Thing Modular. "
                        "Open source (AGPLv3), clean-room implementation from the published documentation.",
                        juce::dontSendNotification);

    struct SliderSetup { juce::Slider* s; juce::Label* c; const char* text; };
    for (auto& e : { SliderSetup { &stationSlider,   &stationCaption,   "Station" },
                     SliderSetup { &crossfadeSlider, &crossfadeCaption, "Crossfade" },
                     SliderSetup { &volumeSlider,    &volumeCaption,    "Volume" } })
    {
        addAndMakeVisible (e.s);
        addAndMakeVisible (e.c);
        e.s->setSliderStyle (juce::Slider::LinearHorizontal);
        e.c->setText (e.text, juce::dontSendNotification);
    }

    stationSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    crossfadeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 20);
    crossfadeSlider.setTextValueSuffix (" ms");
    volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 20);
    volumeSlider.setTextValueSuffix (" dB");

    refreshStatus();
    startTimerHz (10);
}

RadioMusicEditor::~RadioMusicEditor()
{
    stopTimer();
}

void RadioMusicEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1c1e));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("Radio Music (unofficial)", 16, 8, getWidth() - 32, 24, juce::Justification::centredLeft);
}

void RadioMusicEditor::resized()
{
    auto r = getLocalBounds().reduced (16);
    r.removeFromTop (28);

    auto row = r.removeFromTop (28);
    chooseButton.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (8);
    folderLabel.setBounds (row);

    r.removeFromTop (10);
    stationLabel.setBounds (r.removeFromTop (30));
    statusLabel.setBounds (r.removeFromTop (20));
    r.removeFromTop (8);

    for (auto& pair : { std::pair<juce::Label*, juce::Slider*> { &stationCaption,   &stationSlider },
                        std::pair<juce::Label*, juce::Slider*> { &crossfadeCaption, &crossfadeSlider },
                        std::pair<juce::Label*, juce::Slider*> { &volumeCaption,    &volumeSlider } })
    {
        auto line = r.removeFromTop (30);
        pair.first->setBounds (line.removeFromLeft (80));
        pair.second->setBounds (line);
    }

    r.removeFromTop (8);
    aboutLabel.setBounds (r);
}

void RadioMusicEditor::chooseFolder()
{
    const auto start = processor.getSampleFolder() != juce::File()
                           ? processor.getSampleFolder()
                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose the folder of audio files (WAV or AIFF)", start);

    juce::Component::SafePointer<RadioMusicEditor> safe (this);

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

void RadioMusicEditor::timerCallback()
{
    refreshStatus();
}

void RadioMusicEditor::refreshStatus()
{
    const auto folder = processor.getSampleFolder();
    folderLabel.setText (folder == juce::File() ? "No folder chosen" : folder.getFullPathName(),
                         juce::dontSendNotification);

    shownLibrary = processor.getLibrary();
    const auto& engine = processor.getEngine();
    const int count = engine.getNumStations();
    const int idx   = engine.getDisplayedStation();

    if (folder == juce::File())
    {
        stationLabel.setText ("Choose a folder of WAV or AIFF files", juce::dontSendNotification);
    }
    else if (shownLibrary == nullptr)
    {
        stationLabel.setText ("Scanning...", juce::dontSendNotification);
    }
    else if (shownLibrary->stations.empty())
    {
        stationLabel.setText ("No playable WAV or AIFF files found in that folder", juce::dontSendNotification);
    }
    else if (idx >= 0 && idx < (int) shownLibrary->stations.size())
    {
        stationLabel.setText ("Station " + juce::String (idx + 1) + " / " + juce::String (count)
                                  + ":  " + shownLibrary->stations[(size_t) idx].file.getFileName(),
                              juce::dontSendNotification);
    }

    statusLabel.setText ("Radio clock: " + juce::String (engine.getClockSeconds(), 1)
                             + " s   |   buffer underruns: " + juce::String ((int) engine.getUnderrunCount()),
                         juce::dontSendNotification);
}
