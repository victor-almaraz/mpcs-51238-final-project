#include "devices.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <proj/primitives/engine.h>
#include <proj/primitives/io.h>
#include <proj/protocol/jthread.h>

#include "deviceBlock.h"

namespace server {

double hostSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

void listDevices() {
    juce::AudioDeviceManager manager;
    for (auto* type : manager.getAvailableDeviceTypes()) {
        type->scanForDevices();
        std::cout << "audio " << type->getTypeName() << ":\n";
        for (const auto& name : type->getDeviceNames(false)) std::cout << "  output  " << name << "\n";
        for (const auto& name : type->getDeviceNames(true)) std::cout << "  input   " << name << "\n";
    }
    std::cout << "midi:\n";
    for (const auto& d : juce::MidiInput::getAvailableDevices()) std::cout << "  input   " << d.name << "\n";
    for (const auto& d : juce::MidiOutput::getAvailableDevices()) std::cout << "  output  " << d.name << "\n";
}

// JUCE passes the channel arrays as raw pointers: the narrow unsafe use
// at the driver boundary (see the modern-cpp skill). runDeviceBlock turns
// them into spans.
void audioCallback::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&) {
    const auto count = [](auto channels, int n) {
        return channels ? static_cast<std::size_t>(std::max(n, 0)) : std::size_t{0};
    };
    runDeviceBlock(engine_, std::span<const float* const>(in, count(in, numIn)),
                   std::span<float* const>(out, count(out, numOut)),
                   static_cast<std::size_t>(std::max(numSamples, 0)), hostSeconds());
}

audioCallbackRegistration::audioCallbackRegistration(juce::AudioDeviceManager& devices,
                                                     juce::AudioIODeviceCallback& callback)
    : devices_(devices), callback_(callback) {
    devices_.addAudioCallback(&callback_.get());  // JUCE registers callbacks by address
}

audioCallbackRegistration::~audioCallbackRegistration() { devices_.removeAudioCallback(&callback_.get()); }

void midiInputs::rescan() {
    for (const auto& d : juce::MidiInput::getAvailableDevices()) {
        if (open_.contains(d.identifier)) continue;
        // JUCE takes the callback by address; this object outlives the input it owns.
        if (auto input = juce::MidiInput::openDevice(d.identifier, this)) {
            input->start();
            open_[d.identifier] = std::move(input);
        }
    }
}

void midiInputs::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& m) {
    const int size = m.getRawDataSize();
    if (size < 1 || size > 3 || m.isSysEx()) return;
    primitives::midiPacket p;
    p.time = m.getTimeStamp();
    p.size = static_cast<std::uint8_t>(size);
    // JUCE hands the message bytes as a raw pointer and a size.
    const std::span raw(m.getRawData(), static_cast<std::size_t>(size));
    std::ranges::copy(raw, p.bytes.begin());
    io_.deliverMidi(source->getName().toStdString(), p);
}

bool midiOutputs::send(std::string_view pattern, const primitives::midiPacket& p) {
    const std::string key(pattern);
    if (missing_.contains(key)) return false;
    auto it = open_.find(key);
    if (it == open_.end()) {
        std::unique_ptr<juce::MidiOutput> output;
        for (const auto& d : juce::MidiOutput::getAvailableDevices()) {
            if (primitives::ioHub::matches(pattern, d.name.toStdString())) {
                output = juce::MidiOutput::openDevice(d.identifier);
                break;
            }
        }
        if (!output) {
            missing_.insert(key);  // until the next rescan()
            return false;
        }
        it = open_.emplace(key, std::move(output)).first;
    }
    it->second->sendMessageNow(juce::MidiMessage(p.bytes.data(), p.size));
    return true;
}

void midiOutputs::rescan() { missing_.clear(); }

timerDevice::timerDevice(primitives::engine& e, int outputs)
    : engine_(e),
      buffers_(static_cast<std::size_t>(outputs), std::vector<float>(static_cast<std::size_t>(e.info().blockSize))),
      thread_([this](const proj::stop_token& stop) { run(stop); }) {}

void timerDevice::run(const proj::stop_token& stop) {
    using clock = std::chrono::steady_clock;
    const std::vector<std::span<float>> outs(buffers_.begin(), buffers_.end());
    const int n = engine_.info().blockSize;
    const auto period = std::chrono::duration<double>(n / engine_.info().sampleRate);
    auto next = clock::now();
    while (!stop.stop_requested()) {
        engine_.process({.inputs = {}, .outputs = outs}, n, hostSeconds());
        next += std::chrono::duration_cast<clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

} // namespace server
