#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <ixwebsocket/IXWebSocket.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

// Plays Magenta RealTime 2 running on a GPU server (the jam app from magenta.yaml on enki):
// held MIDI notes and settings go up a WebSocket, 48 kHz stereo audio streams back.
class MRT2RemoteProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    MRT2RemoteProcessor();
    ~MRT2RemoteProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::String getServer() const;
    juce::String getPrompt() const;
    juce::String getStatus() const;
    void setServer (const juce::String&);
    void setPrompt (const juce::String&);

    juce::AudioProcessorValueTreeState params;

private:
    void timerCallback() override;
    void connect();
    void onSocket (const ix::WebSocketMessagePtr&);
    void onAudio (const std::string& payload);
    juce::String makeSet() const;
    void setStatus (const juce::String&);

    ix::WebSocket ws;
    std::thread connector;          // connect() does a blocking HTTP call, so it stays off the message thread
    std::atomic<bool> connecting { false }, needReconnect { true }, wrongRate { false };
    juce::uint32 lastAttemptMs = 0;
    juce::String lastSet;           // message thread only

    mutable std::mutex textLock;    // guards the strings below; never taken on the audio thread
    juce::String server { "http://enki.local:7860" }, prompt { "warm electric piano" }, status { "starting" }, sessionId;

    std::array<std::atomic<bool>, 128> held {};   // written by the audio thread, read by the timer
    std::atomic<float>* gain = nullptr;

    // network thread -> audio thread: interleaved stereo float at 48 kHz, 1 s deep
    static constexpr int fifoFrames = 48000;
    juce::AbstractFifo fifo { fifoFrames };
    std::vector<float> fifoData = std::vector<float> (fifoFrames * 2);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MRT2RemoteProcessor)
};
