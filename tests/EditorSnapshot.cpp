// Developer tool (not part of the plug-in): renders the plug-in's editor to PNG files, without a screen,
// so the look of the interface can be checked from a build machine. Enabled with -DRM_BUILD_SNAPSHOT=ON.
//
//   EditorSnapshot <output folder>
#include "Plugin/PluginEditor.h"
#include <chrono>
#include <thread>

static void writeTone (const juce::File& file, double rate, double seconds, double freq)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    auto w = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                                         .withNumChannels (1)
                                                                         .withBitsPerSample (16));
    const int64_t len = (int64_t) (seconds * rate);
    juce::AudioBuffer<float> buf (1, 4096);
    for (int64_t pos = 0; pos < len; pos += 4096)
    {
        const int n = (int) std::min<int64_t> (4096, len - pos);
        for (int i = 0; i < n; ++i)
            buf.setSample (0, i, 0.4f * (float) std::sin (2.0 * 3.14159265358979 * freq * (double) (pos + i) / rate));
        w->writeFromAudioSampleBuffer (buf, 0, n);
    }
}

static void pump (RadioMusicProcessor& p, double seconds)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    const auto t0 = std::chrono::steady_clock::now();
    const int blocks = (int) (seconds * 48000.0 / 512.0);
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        p.processBlock (buf, midi);
        std::this_thread::sleep_until (t0 + std::chrono::duration<double> ((b + 1) * 512.0 / 48000.0));
    }
}

static void snap (RadioMusicProcessor& p, const juce::File& png)
{
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setVisible (true);
    juce::Thread::sleep (50);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::PNGImageFormat fmt;
    png.deleteFile();
    juce::FileOutputStream out (png);
    fmt.writeImageToStream (img, out);
    std::cout << "wrote " << png.getFullPathName() << " (" << img.getWidth() << "x" << img.getHeight() << ")\n";
}

static void setParam (RadioMusicProcessor& p, const char* id, float realValue)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (realValue));
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::String ("."));
    outDir.createDirectory();

    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rm_snapshot");
    root.deleteRecursively();
    auto folder = root.getChildFile ("Field Recordings"); folder.createDirectory();
    const char* names[] = { "Would it look the same 2.aif", "BBC Radio 3 evening", "Shortwave 6 MHz night", "Numbers station",
                            "Tape loop 01", "Tape loop 02", "Street market Lisbon", "Late night phone-in", "Ferry horn", "Static and hiss", "Choir rehearsal", "Last station" };
    int k = 0;
    for (auto* n : names)
    {
        auto f = folder.getChildFile (juce::String (n).replace (".aif", "") + ".wav");
        writeTone (f, 48000.0, 40.0 + 3.0 * k, 110.0 * std::pow (2.0, k / 12.0));
        ++k;
    }

    RadioMusicProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    // Developer check of the whole plug-in object (parameters, settings, engine): Reset on a "next" subfolder.
    if (argc > 2 && juce::String (argv[2]) == "resettest")
    {
        auto bank = root.getChildFile ("Radio Test").getChildFile ("0"); bank.createDirectory();
        writeTone (bank.getChildFile ("02 one.wav"), 48000.0, 20.0, 220.0);
        writeTone (bank.getChildFile ("03 two.wav"), 48000.0, 20.0, 330.0);
        auto nxt = bank.getChildFile ("loops next"); nxt.createDirectory();
        writeTone (nxt.getChildFile ("04 x.wav"), 48000.0, 20.0, 440.0);
        writeTone (nxt.getChildFile ("05 y.wav"), 48000.0, 20.0, 550.0);
        writeTone (nxt.getChildFile ("06 z.wav"), 48000.0, 20.0, 660.0);
        root.getChildFile ("Radio Test").getChildFile ("settings.txt").replaceWithText ("loopMode = 0\n");

        proc.setSampleFolder (root.getChildFile ("Radio Test"));
        for (int i = 0; i < 100 && proc.getLibrary() == nullptr; ++i) juce::Thread::sleep (50);
        auto lib = proc.getLibrary();
        std::cout << "slots " << (lib ? (int) lib->slots.size() : -1) << " files " << (lib ? (int) lib->stations.size() : -1) << "\n";

        // Bryn's screenshot: radio start off, 90s mode, 1 ms crossfade
        if (argc > 3 && juce::String (argv[3]) == "bryn")
        {
            setParam (proc, "radioStart", 0.0f);
            setParam (proc, "speedMode", 2.0f);
            setParam (proc, "crossfade", 1.0f);
            setParam (proc, "volume", -13.8f);
        }
        setParam (proc, "station", 0.9f);   // last of 3 slots
        pump (proc, 1.0);
        std::cout << "slot " << proc.getEngine().getDisplayedStation() << " of " << proc.getEngine().getNumStations()
                  << " file " << proc.getEngine().getDisplayedFile() << "\n";
        for (int i = 0; i < 4; ++i)
        {
            proc.triggerReset();
            pump (proc, 0.6);
            std::cout << "after reset " << i + 1 << ": file " << proc.getEngine().getDisplayedFile() << "\n";
        }
        return 0;
    }

    // MIDI check: a note-on for note 36 is a Reset, other notes are not.
    if (argc > 2 && juce::String (argv[2]) == "miditest")
    {
        proc.setSampleFolder (folder);
        for (int i = 0; i < 100 && proc.getLibrary() == nullptr; ++i) juce::Thread::sleep (50);
        setParam (proc, "station", 0.3f);
        pump (proc, 0.5);

        auto sendNote = [&] (int note, bool on)
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (on ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, note), 10);
            buf.clear();
            proc.processBlock (buf, midi);
        };

        const auto before = proc.getEngine().getResetsApplied();
        sendNote (60, true);  sendNote (60, false);
        const auto afterOther = proc.getEngine().getResetsApplied();
        sendNote (36, true);  sendNote (36, false);
        sendNote (36, true);
        const auto afterReset = proc.getEngine().getResetsApplied();
        std::cout << "resets: start " << before << ", after note 60: " << afterOther << ", after two note-ons for 36: " << afterReset << "\n";
        std::cout << ((afterOther == before && afterReset == before + 2) ? "MIDI OK" : "MIDI FAIL") << "\n";
        return (afterOther == before && afterReset == before + 2) ? 0 : 1;
    }

    // 1. empty state
    snap (proc, outDir.getChildFile ("1_no_folder.png"));

    // 2. playing, loop window set
    proc.setSampleFolder (folder);
    for (int i = 0; i < 100 && proc.getLibrary() == nullptr; ++i) juce::Thread::sleep (50);
    setParam (proc, "station", 0.30f);
    setParam (proc, "start", 0.20f);
    setParam (proc, "end", 0.55f);
    setParam (proc, "crossfade", 1500.0f);
    setParam (proc, "volume", -6.0f);
    setParam (proc, "pause", 0.0f);
    setParam (proc, "speedTape", 150.0f);
    pump (proc, 2.5);
    snap (proc, outDir.getChildFile ("2_playing.png"));

    // 3. reversed window, ping-pong, a few options on
    setParam (proc, "station", 0.80f);
    setParam (proc, "start", 0.85f);
    setParam (proc, "end", 0.35f);
    setParam (proc, "loopMode", 2.0f);
    setParam (proc, "pause", 1.0f);
    setParam (proc, "hostSync", 1.0f);
    setParam (proc, "radioStart", 0.0f);
    setParam (proc, "crossfade", 25.0f);
    setParam (proc, "speedMode", 1.0f);
    setParam (proc, "speedNotes", 7.0f);
    setParam (proc, "gritMode", 1.0f);
    pump (proc, 1.5);
    snap (proc, outDir.getChildFile ("3_reverse_pingpong_notes.png"));

    // 4. 90s mode (not available yet: speed knob dimmed)
    setParam (proc, "speedMode", 2.0f);
    setParam (proc, "pause", 0.0f);
    pump (proc, 0.3);
    snap (proc, outDir.getChildFile ("4_90s.png"));

    // 5. radio tuner mode (radio knobs active) and soft mode (crossfade knob dimmed)
    setParam (proc, "speedMode", 0.0f);
    setParam (proc, "tunerMode", 2.0f);
    setParam (proc, "station", 0.30f);
    pump (proc, 0.5);
    snap (proc, outDir.getChildFile ("6_radio.png"));
    setParam (proc, "tunerMode", 1.0f);
    pump (proc, 0.3);
    snap (proc, outDir.getChildFile ("7_soft.png"));
    setParam (proc, "tunerMode", 0.0f);

    // About screen
    {
        rmui::AboutOverlay about;
        about.setSize (RadioMusicEditor::kBaseWidth, RadioMusicEditor::kBaseHeight);
        auto img = about.createComponentSnapshot (about.getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat fmt;
        auto f = outDir.getChildFile ("8_about.png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        fmt.writeImageToStream (img, out);
        std::cout << "wrote " << f.getFullPathName() << "\n";
    }

    // scaled window (1.5x): panel must fill it
    {
        setParam (proc, "speedMode", 1.0f);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        ed->setSize (1410, 1026);
        ed->setVisible (true);
        juce::Thread::sleep (50);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat fmt;
        auto f = outDir.getChildFile ("5_scaled.png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        fmt.writeImageToStream (img, out);
        std::cout << "wrote " << f.getFullPathName() << " (" << img.getWidth() << "x" << img.getHeight() << ")\n";
    }

    root.deleteRecursively();
    return 0;
}
