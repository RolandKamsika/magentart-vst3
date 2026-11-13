cmake_minimum_required(VERSION 3.22)
project(MRT2Remote VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
include(FetchContent)

FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW ON)
# IXWebSocket's CMakeLists predates CMake 3.5, which CMake 4 refuses without this
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git GIT_TAG v11.4.5 GIT_SHALLOW ON)
FetchContent_MakeAvailable(juce ixwebsocket)

juce_add_plugin(MRT2Remote
    COMPANY_NAME "rjones-projects"
    PLUGIN_MANUFACTURER_CODE Rjpr
    PLUGIN_CODE Mrt2
    FORMATS VST3
    PRODUCT_NAME "MRT2 Remote"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Instrument Synth)

target_sources(MRT2Remote PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(MRT2Remote PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MRT2Remote
    PRIVATE ixwebsocket juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# End-to-end check: loads the built .vst3 and confirms audio streams back from the server
juce_add_console_app(MRT2Harness PRODUCT_NAME "MRT2 Harness")
target_sources(MRT2Harness PRIVATE tests/Harness.cpp)
target_compile_definitions(MRT2Harness PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(MRT2Harness PRIVATE juce::juce_audio_processors juce::juce_audio_formats PUBLIC juce::juce_recommended_config_flags)
