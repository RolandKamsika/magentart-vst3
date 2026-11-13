// End-to-end checks: load the built VST3 like a host would and drive it in real time.
//   MRT2Harness "<path to MRT2 Remote.vst3>" [seconds]          stream check: audio arrives without dropouts
//   MRT2Harness "<path to MRT2 Remote.vst3>" latency <out.wav>  note-to-sound latency, in solo mode without drums
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <iostream>
#include <thread>

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 512;

void setParam (juce::AudioPluginInstance& plugin, const juce::String& name, float value)
{
    for (auto* p : plugin.getParameters())
        if (p->getName (64) == name)
            return p->setValueNotifyingHost (value);
    std::cerr << "warning: no parameter named " << name << "\n";
}

float median (std::vector<float> v)
{
    if (v.empty())
        return 0.0f;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Runs the plugin in real time on its own thread; noteAt(block) returns +1 to press the chord, -1 to release it.
std::vector<float> runRealtime (juce::AudioPluginInstance& plugin, int totalBlocks,
                                std::function<int (int)> noteAt, juce::AudioBuffer<float>* record)
{
    std::vector<float> rms;
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        auto next = std::chrono::steady_clock::now();
        for (int b = 0; b < totalBlocks; ++b)
        {
            midi.clear();
            if (const int e = noteAt (b); e != 0)
                for (int note : { 60, 64, 67 })
                    midi.addEvent (e > 0 ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100)
                                         : juce::MidiMessage::noteOff (1, note), 0);
            plugin.processBlock (buffer, midi);
            rms.push_back (std::max (buffer.getRMSLevel (0, 0, block), buffer.getRMSLevel (1, 0, block)));
            if (record != nullptr)
                for (int ch = 0; ch < 2; ++ch)
                    record->copyFrom (ch, b * block, buffer, ch, 0, block);
            next += std::chrono::microseconds ((long long) (block * 1.0e6 / rate));
            std::this_thread::sleep_until (next);
        }
        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    });
    // the plugin's timers run on this thread's message loop, as in a DAW
    juce::MessageManager::getInstance()->runDispatchLoop();
    audio.join();
    return rms;
}

int streamCheck (juce::AudioPluginInstance& plugin, double seconds)
{
    const int totalBlocks = (int) (seconds * rate / block);
    auto rms = runRealtime (plugin, totalBlocks, [] (int b) { return b == 0 ? 1 : 0; }, nullptr);
    auto first = std::find_if (rms.begin(), rms.end(), [] (float r) { return r > 1.0e-4f; });
    if (first == rms.end())
    {
        std::cout << "FAIL: no audio in " << seconds << " s\n";
        return 1;
    }
    const auto loud = std::count_if (first, rms.end(), [] (float r) { return r > 1.0e-4f; });
    const double coverage = 100.0 * (double) loud / (double) (rms.end() - first);
    std::cout << "first audio after " << (first - rms.begin()) * block / rate << " s; "
              << coverage << "% of blocks after that had audio\n";
    std::cout << (coverage >= 90.0 ? "PASS" : "FAIL: too many silent blocks (dropouts)") << "\n";
    return coverage >= 90.0 ? 0 : 1;
}

int latencyCheck (juce::AudioPluginInstance& plugin, const juce::File& wavOut)
{
    setParam (plugin, "Solo", 1.0f);
    setParam (plugin, "Drums", 0.0f);

    const int perSecond = (int) (rate / block);
    const int warmup = 8 * perSecond, hold = 3 * perSecond, rest = 3 * perSecond, trials = 6;
    const int totalBlocks = warmup + trials * (hold + rest);
    std::vector<int> onsets;
    for (int t = 0; t < trials; ++t)
        onsets.push_back (warmup + t * (hold + rest));

    juce::AudioBuffer<float> recording (2, totalBlocks * block);
    auto rms = runRealtime (plugin, totalBlocks, [&] (int b)
    {
        for (int on : onsets)
        {
            if (b == on) return 1;
            if (b == on + hold) return -1;
        }
        return 0;
    }, &recording);

    if (auto stream = wavOut.createOutputStream())
    {
        juce::WavAudioFormat wav;
        if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), rate, 2, 16, {}, 0)))
        {
            stream.release();   // the writer owns it now
            writer->writeFromAudioSampleBuffer (recording, 0, recording.getNumSamples());
        }
    }

    // onset = first moment the level stays above 4x the quiet level before the note (3 blocks in a row)
    std::vector<double> latencies;
    for (int t = 0; t < trials; ++t)
    {
        const int on = onsets[(size_t) t];
        const float quiet = median ({ rms.begin() + on - perSecond, rms.begin() + on });
        const float held = median ({ rms.begin() + on + perSecond, rms.begin() + on + hold });
        const float threshold = std::max (quiet * 4.0f, quiet + 0.25f * (held - quiet));
        int found = -1;
        for (int b = on; b + 2 < on + hold && found < 0; ++b)
            if (rms[(size_t) b] > threshold && rms[(size_t) b + 1] > threshold && rms[(size_t) b + 2] > threshold)
                found = b;
        std::cout << "trial " << t + 1 << ": quiet " << quiet << ", held " << held;
        if (found < 0 || held < quiet * 4.0f)
            std::cout << " -> no clear onset\n";
        else
        {
            latencies.push_back ((found - on) * block / rate * 1000.0);
            std::cout << " -> " << latencies.back() << " ms\n";
        }
    }
    std::cout << "recording: " << wavOut.getFullPathName() << "\n";
    if (latencies.size() < 3)
    {
        std::cout << "FAIL: only " << latencies.size() << " of " << trials << " trials had a clear onset\n";
        return 1;
    }
    std::sort (latencies.begin(), latencies.end());
    std::cout << "note-to-sound latency: median " << latencies[latencies.size() / 2] << " ms, min "
              << latencies.front() << " ms, max " << latencies.back() << " ms (" << latencies.size() << " trials)\n";
    return 0;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2)
    {
        std::cerr << "usage: MRT2Harness <path to .vst3> [seconds | latency <out.wav>]\n";
        return 2;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, argv[1]);
    if (types.isEmpty())
    {
        std::cerr << "FAIL: no plugin found in " << argv[1] << "\n";
        return 1;
    }
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*types[0], rate, block, error);
    if (plugin == nullptr)
    {
        std::cerr << "FAIL: could not load plugin: " << error << "\n";
        return 1;
    }
    plugin->enableAllBuses();
    plugin->prepareToPlay (rate, block);

    const juce::String mode = argc > 2 ? argv[2] : "";
    const int result = mode == "latency"
                           ? latencyCheck (*plugin, juce::File::getCurrentWorkingDirectory().getChildFile (argc > 3 ? argv[3] : "latency.wav"))
                           : streamCheck (*plugin, mode.isNotEmpty() ? mode.getDoubleValue() : 15.0);
    plugin->releaseResources();
    plugin.reset();
    return result;
}
