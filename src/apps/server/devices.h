// JUCE device glue for the server: the device clock, the audio
// callback and its registration, MIDI inputs and outputs, a timer that
// stands in for an audio device, and device listing.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include <espresso/primitives/engine.h>
#include <espresso/protocol/jthread.h>

namespace server {

/// The device clock shared by audio callbacks and MIDI timestamps, in seconds.
double hostSeconds();

/// Prints audio and MIDI devices to standard output, by device type.
void listDevices();

/// Chooses the device type that @p devices opens at its next
/// `initialise`, and opens no device. JACK (PipeWire's JACK interface,
/// or a JACK server) is chosen when it lists an output device, and lists
/// @p device among its outputs when @p device is not empty; otherwise
/// the manager keeps its default type, the first that lists a device.
/// Returns a warning when a JACK type exists and another type is kept;
/// an empty string otherwise, as on macOS, which has no JACK type.
std::string preferJack(juce::AudioDeviceManager& devices, const std::string& device);

/// Runs the engine from an audio device.
class audioCallback final : public juce::AudioIODeviceCallback {
public:
    /// Drives @p e, which must outlive the callback, on the device's first
    /// @p outputs output channels; the others are silenced.
    audioCallback(primitives::engine& e, int outputs)
        : engine_(e), outputs_(static_cast<std::size_t>(outputs)) {}
    /// Processes one device buffer.
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    /// Nothing to prepare: the engine is built for the device's settings.
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    /// Nothing to release.
    void audioDeviceStopped() override {}

private:
    primitives::engine& engine_;
    std::size_t outputs_;
};

/// Registers an audio callback with a device manager for its lifetime.
/// Declare it after the callback and everything the callback uses, so
/// the device stops calling it before they are destroyed.
class audioCallbackRegistration {
public:
    /// Adds @p callback to @p devices; both must outlive the registration.
    audioCallbackRegistration(juce::AudioDeviceManager& devices, juce::AudioIODeviceCallback& callback);
    /// Removes the callback. JUCE returns once the device is no longer in it.
    ~audioCallbackRegistration();

    audioCallbackRegistration(const audioCallbackRegistration&) = delete;
    audioCallbackRegistration& operator=(const audioCallbackRegistration&) = delete;

private:
    juce::AudioDeviceManager& devices_;
    std::reference_wrapper<juce::AudioIODeviceCallback> callback_;
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
    /// Returns false when no output matches; the caller counts the drop.
    /// A pattern that matched nothing is not looked up again until
    /// rescan().
    [[nodiscard]] bool send(std::string_view pattern, const primitives::midiPacket& p);
    /// Forgets the patterns that matched no output, so devices plugged in
    /// since are found.
    void rescan();

private:
    std::map<std::string, std::unique_ptr<juce::MidiOutput>> open_;
    std::set<std::string> missing_;
};

/// Runs the engine in real time without a device (--no-audio).
class timerDevice {
public:
    /// Starts processing @p e into @p outputs discarded channels. The
    /// thread stops and joins when the device is destroyed.
    timerDevice(primitives::engine& e, int outputs);

private:
    void run(const espresso::stop_token& stop);

    primitives::engine& engine_;
    std::vector<std::vector<float>> buffers_;
    espresso::jthread thread_;  // last: stops before the buffers go away
};

} // namespace server
