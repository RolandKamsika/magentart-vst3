#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <ixwebsocket/IXNetSystem.h>

#include <cstring>

namespace
{
constexpr double modelRate = 48000.0;   // the model only produces 48 kHz; no resampling here

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using Float = juce::AudioParameterFloat;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<Float> (juce::ParameterID { "temperature", 1 }, "Temperature",
                                         juce::NormalisableRange<float> (0.5f, 2.0f, 0.01f), 1.1f));
    layout.add (std::make_unique<Float> (juce::ParameterID { "cfg", 1 }, "Style strength",
                                         juce::NormalisableRange<float> (0.0f, 8.0f, 0.1f), 1.6f));
    layout.add (std::make_unique<Float> (juce::ParameterID { "cfg_notes", 1 }, "Note strength",
                                         juce::NormalisableRange<float> (0.0f, 7.0f, 0.1f), 2.4f));
    layout.add (std::make_unique<Float> (juce::ParameterID { "gain", 1 }, "Gain",
                                         juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f), 1.0f));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "buffer", 1 }, "Server buffer",
                                                              juce::StringArray { "150 ms", "300 ms", "500 ms" }, 0));
    // solo: the model only plays the held notes and goes quiet when none are held
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "solo", 1 }, "Solo", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "drums", 1 }, "Drums", true));
    return layout;
}
} // namespace

MRT2RemoteProcessor::MRT2RemoteProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "MRT2Remote", makeLayout())
{
    gain = params.getRawParameterValue ("gain");
    ix::initNetSystem();
    ws.disableAutomaticReconnection();   // the server ends the session when the socket closes, so reconnect = new session
    ws.setOnMessageCallback ([this] (const ix::WebSocketMessagePtr& msg) { onSocket (msg); });
    startTimer (10);
}

MRT2RemoteProcessor::~MRT2RemoteProcessor()
{
    stopTimer();
    if (connector.joinable())
        connector.join();
    ws.stop();
    ix::uninitNetSystem();
}

void MRT2RemoteProcessor::prepareToPlay (double sampleRate, int)
{
    wrongRate = sampleRate != modelRate;
}

bool MRT2RemoteProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MRT2RemoteProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())
            held[(size_t) m.getNoteNumber()] = true;
        else if (m.isNoteOff())
            held[(size_t) m.getNoteNumber()] = false;
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            for (auto& h : held)
                h = false;
    }

    buffer.clear();
    if (wrongRate)
        return;

    const int n = buffer.getNumSamples();
    int start1, size1, start2, size2;
    fifo.prepareToRead (n, start1, size1, start2, size2);

    const float g = gain->load();
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    auto take = [&] (int start, int count, int offset)
    {
        for (int i = 0; i < count; ++i)
        {
            left[offset + i] = fifoData[(size_t) (start + i) * 2] * g;
            right[offset + i] = fifoData[(size_t) (start + i) * 2 + 1] * g;
        }
    };
    take (start1, size1, 0);
    take (start2, size2, size1);
    fifo.finishedRead (size1 + size2);   // an underrun just leaves the rest of the block silent
}

void MRT2RemoteProcessor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (needReconnect && ! connecting && now - lastAttemptMs > 2000)
    {
        lastAttemptMs = now;
        if (connector.joinable())
            connector.join();
        connecting = true;
        connector = std::thread ([this] { connect(); connecting = false; });
    }

    // the session id is part of the message, so a new session always gets the current settings
    if (ws.getReadyState() == ix::ReadyState::Open)
    {
        auto msg = makeSet();
        if (msg != lastSet)
        {
            ws.sendText (msg.toStdString());
            lastSet = msg;
        }
    }
}

void MRT2RemoteProcessor::connect()
{
    ws.stop();   // synchronous: the old socket's callbacks have finished after this
    const auto base = getServer().trim().trimCharactersAtEnd ("/");
    const auto sid = "vst-" + juce::Uuid().toString();
    setStatus ("connecting to " + base);

    auto* body = new juce::DynamicObject();
    body->setProperty ("data", juce::Array<juce::var> { juce::var (sid) });
    int code = 0;
    auto stream = juce::URL (base + "/gradio_api/call/start")
                      .withPOSTData (juce::JSON::toString (juce::var (body)))
                      .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                              .withExtraHeaders ("Content-Type: application/json")
                                              .withConnectionTimeoutMs (5000)
                                              .withStatusCode (&code));
    if (stream == nullptr || code != 200)
    {
        setStatus ("can't reach " + base + " - is magenta.sh up running on the server?");
        needReconnect = true;
        return;
    }

    {
        std::lock_guard<std::mutex> lock (textLock);
        sessionId = sid;
    }
    const auto wsBase = base.replace ("https://", "wss://").replace ("http://", "ws://");
    ws.setUrl ((wsBase + "/ws?session_id=" + sid).toStdString());
    needReconnect = false;
    ws.start();
}

void MRT2RemoteProcessor::onSocket (const ix::WebSocketMessagePtr& msg)
{
    switch (msg->type)
    {
        case ix::WebSocketMessageType::Open:
            setStatus ("streaming");
            break;
        case ix::WebSocketMessageType::Message:
            if (msg->binary)
                onAudio (msg->str);
            else if (msg->str.find ("\"ended\"") != std::string::npos)
                needReconnect = true;
            break;
        case ix::WebSocketMessageType::Close:
            setStatus ("disconnected (" + juce::String (msg->closeInfo.reason) + "), retrying");
            needReconnect = true;
            break;
        case ix::WebSocketMessageType::Error:
            setStatus ("connection error: " + juce::String (msg->errorInfo.reason));
            needReconnect = true;
            break;
        default:
            break;
    }
}

void MRT2RemoteProcessor::onAudio (const std::string& payload)
{
    // payload: float32 server frame time, then interleaved int16 stereo
    if (payload.size() < 8)
        return;
    const int frames = (int) ((payload.size() - 4) / 4);

    int start1, size1, start2, size2;
    fifo.prepareToWrite (frames, start1, size1, start2, size2);   // a full FIFO drops the newest audio
    auto put = [&] (int start, int count, int offset)
    {
        for (int i = 0; i < count; ++i)
        {
            int16_t lr[2];
            std::memcpy (lr, payload.data() + 4 + (size_t) (offset + i) * 4, sizeof lr);
            fifoData[(size_t) (start + i) * 2] = lr[0] / 32768.0f;
            fifoData[(size_t) (start + i) * 2 + 1] = lr[1] / 32768.0f;
        }
    };
    put (start1, size1, 0);
    put (start2, size2, size1);
    fifo.finishedWrite (size1 + size2);

    // the server slows down when the client reports a deep queue
    auto* lead = new juce::DynamicObject();
    lead->setProperty ("type", "buffer");
    {
        std::lock_guard<std::mutex> lock (textLock);
        lead->setProperty ("session_id", sessionId);
    }
    lead->setProperty ("lead", (int) (fifo.getNumReady() * 1000.0 / modelRate));
    ws.sendText (juce::JSON::toString (juce::var (lead), true).toStdString());
}

juce::String MRT2RemoteProcessor::makeSet() const
{
    juce::Array<juce::var> notes;
    for (int i = 0; i < 128; ++i)
        if (held[(size_t) i])
            notes.add (i);

    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "set");
    {
        std::lock_guard<std::mutex> lock (textLock);
        o->setProperty ("session_id", sessionId);
        o->setProperty ("prompts", juce::Array<juce::var> { juce::var (prompt) });
    }
    o->setProperty ("weights", juce::Array<juce::var> { juce::var (1.0) });
    o->setProperty ("notes", notes);
    o->setProperty ("temperature", params.getRawParameterValue ("temperature")->load());
    o->setProperty ("top_k", 50);
    o->setProperty ("cfg", params.getRawParameterValue ("cfg")->load());
    o->setProperty ("cfg_notes", params.getRawParameterValue ("cfg_notes")->load());
    o->setProperty ("cfg_drums", 4.0);
    o->setProperty ("model", "mrt2_small");
    o->setProperty ("buffer", (int) params.getRawParameterValue ("buffer")->load());
    o->setProperty ("unmaskwidth", params.getRawParameterValue ("solo")->load() > 0.5f ? 127 : 0);
    o->setProperty ("drumless", params.getRawParameterValue ("drums")->load() < 0.5f);
    return juce::JSON::toString (juce::var (o), true);
}

juce::String MRT2RemoteProcessor::getServer() const { std::lock_guard<std::mutex> lock (textLock); return server; }
juce::String MRT2RemoteProcessor::getPrompt() const { std::lock_guard<std::mutex> lock (textLock); return prompt; }

juce::String MRT2RemoteProcessor::getStatus() const
{
    if (wrongRate)
        return "set the project sample rate to 48 kHz - the model only produces 48 kHz";
    std::lock_guard<std::mutex> lock (textLock);
    return status;
}

void MRT2RemoteProcessor::setStatus (const juce::String& s) { std::lock_guard<std::mutex> lock (textLock); status = s; }

void MRT2RemoteProcessor::setServer (const juce::String& s)
{
    {
        std::lock_guard<std::mutex> lock (textLock);
        if (server == s)
            return;
        server = s;
    }
    needReconnect = true;
    lastAttemptMs = 0;
}

void MRT2RemoteProcessor::setPrompt (const juce::String& s) { std::lock_guard<std::mutex> lock (textLock); prompt = s; }

void MRT2RemoteProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = params.copyState();
    state.setProperty ("server", getServer(), nullptr);
    state.setProperty ("prompt", getPrompt(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MRT2RemoteProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return;
    params.replaceState (state);
    setServer (state.getProperty ("server", getServer()).toString());
    setPrompt (state.getProperty ("prompt", getPrompt()).toString());
}

juce::AudioProcessorEditor* MRT2RemoteProcessor::createEditor() { return new MRT2RemoteEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MRT2RemoteProcessor(); }
