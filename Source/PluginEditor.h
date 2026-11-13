#pragma once

#include "PluginProcessor.h"

class MRT2RemoteEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MRT2RemoteEditor (MRT2RemoteProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    MRT2RemoteProcessor& proc;
    juce::Label serverLabel, promptLabel, bufferLabel, status;
    juce::TextEditor server, prompt;
    std::array<Knob, 4> knobs;
    juce::ComboBox buffer;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> bufferAttachment;
    juce::ToggleButton solo { "Solo" }, drums { "Drums" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> soloAttachment, drumsAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MRT2RemoteEditor)
};
