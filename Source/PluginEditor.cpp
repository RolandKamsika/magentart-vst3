#include "PluginEditor.h"

MRT2RemoteEditor::MRT2RemoteEditor (MRT2RemoteProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    auto setupField = [this] (juce::Label& label, const juce::String& title, juce::TextEditor& field,
                              const juce::String& value, std::function<void (const juce::String&)> apply)
    {
        label.setText (title, juce::dontSendNotification);
        addAndMakeVisible (label);
        field.setText (value, false);
        auto commit = [&field, apply] { apply (field.getText()); };
        field.onReturnKey = commit;
        field.onFocusLost = commit;
        addAndMakeVisible (field);
    };
    setupField (serverLabel, "Server", server, proc.getServer(), [this] (const juce::String& s) { proc.setServer (s); });
    setupField (promptLabel, "Style prompt", prompt, proc.getPrompt(), [this] (const juce::String& s) { proc.setPrompt (s); });

    const std::array<std::pair<const char*, const char*>, 4> knobParams {
        { { "temperature", "Temperature" }, { "cfg", "Style strength" }, { "cfg_notes", "Note strength" }, { "gain", "Gain" } }
    };
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        k.label.setText (knobParams[i].second, juce::dontSendNotification);
        k.label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (k.label);
        addAndMakeVisible (k.slider);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, knobParams[i].first, k.slider);
    }

    bufferLabel.setText ("Server buffer", juce::dontSendNotification);
    addAndMakeVisible (bufferLabel);
    buffer.addItemList ({ "150 ms", "300 ms", "500 ms" }, 1);
    addAndMakeVisible (buffer);
    bufferAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "buffer", buffer);
    addAndMakeVisible (solo);
    addAndMakeVisible (drums);
    soloAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.params, "solo", solo);
    drumsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.params, "drums", drums);

    status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (status);

    setSize (520, 300);
    startTimerHz (5);
    timerCallback();
}

void MRT2RemoteEditor::timerCallback()
{
    status.setText (proc.getStatus(), juce::dontSendNotification);
}

void MRT2RemoteEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MRT2RemoteEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto row = [&area] (juce::Label& label, juce::Component& field)
    {
        auto r = area.removeFromTop (26);
        label.setBounds (r.removeFromLeft (100));
        field.setBounds (r);
        area.removeFromTop (8);
    };
    row (serverLabel, server);
    row (promptLabel, prompt);
    {
        auto r = area.removeFromTop (26);
        bufferLabel.setBounds (r.removeFromLeft (100));
        buffer.setBounds (r.removeFromLeft (120));
        r.removeFromLeft (16);
        solo.setBounds (r.removeFromLeft (80));
        drums.setBounds (r.removeFromLeft (80));
        area.removeFromTop (8);
    }

    status.setBounds (area.removeFromBottom (24));
    const int w = area.getWidth() / (int) knobs.size();
    for (auto& k : knobs)
    {
        auto col = area.removeFromLeft (w);
        k.label.setBounds (col.removeFromTop (20));
        k.slider.setBounds (col.reduced (4));
    }
}
