// JUCE device glue for the server: the device clock, the audio
// callback, MIDI inputs and outputs, a timer that stands in for an audio
// device, and device listing.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include <proj/primitives/engine.h>

namespace server {

/// The device clock shared by audio callbacks and MIDI timestamps, in seconds.
double hostSeconds();

/// Prints audio and MIDI devices to standard output.
void listDevices();

/// Runs the engine from an audio device.
class audioCallback final : public juce::AudioIODeviceCallback {
public:
    /// Drives @p e, which must outlive the callback.
    explicit audioCallback(primitives::engine& e) : engine_(e) {}
    /// Processes one device buffer.
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    /// Nothing to prepare: the engine is built for the device's settings.
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    /// Nothing to release.
    void audioDeviceStopped() override {}

private:
    primitives::engine& engine_;
};

/// Opens every MIDI input and forwards messages to the engine's ports.
class midiInputs final : public juce::MidiInputCallback {
public:
    /// Delivers to @p io.
    explicit midiInputs(primitives::ioHub& io) : io_(io) {}
    /// Opens inputs that appeared since the last scan.
    void rescan();
    /// Called by JUCE on its MIDI thread.
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& m) override;

private:
    primitives::ioHub& io_;
    std::map<juce::String, std::unique_ptr<juce::MidiInput>> open_;
};

/// MIDI outputs opened on demand by device pattern.
class midiOutputs {
public:
    /// Sends @p p to the first output whose name matches @p pattern.
    void send(std::string_view pattern, const primitives::midiPacket& p);

private:
    std::map<std::string, std::unique_ptr<juce::MidiOutput>> open_;
};

/// Runs the engine in real time without a device (--no-audio).
class timerDevice {
public:
    /// Starts processing @p e into @p outputs discarded channels.
    timerDevice(primitives::engine& e, int outputs);
    /// Stops and joins the thread.
    ~timerDevice();

private:
    void run();

    primitives::engine& engine_;
    std::vector<std::vector<float>> buffers_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace server
