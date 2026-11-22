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

#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/protocol/jthread.h>

#include "deviceBlock.h"
#include "realtime.h"

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

std::string preferJack(juce::AudioDeviceManager& devices, const std::string& device) {
    // With no channels needed, initialise scans every type and picks the
    // first that lists a device, and neither it nor the type change below
    // opens one: a device is opened only for the channels a setup needs.
    devices.initialise(0, 0, nullptr, false);
    const juce::String jack("JACK");
    const juce::String wanted = juce::String(device).trim();
    bool exists = false;
    bool listed = false;
    for (const auto* type : devices.getAvailableDeviceTypes()) {
        if (type->getTypeName() != jack) continue;
        exists = true;
        // The comparison JUCE makes when it opens a named device.
        listed = std::ranges::any_of(type->getDeviceNames(false), [&](const juce::String& name) {
            return wanted.isEmpty() || name.trim().equalsIgnoreCase(wanted);
        });
    }
    if (listed) {
        devices.setCurrentAudioDeviceType(jack, false);
        return {};
    }
    const auto kept = devices.getCurrentAudioDeviceType();
    if (!exists || kept.isEmpty() || kept == jack) return {};  // opening reports what is missing
    const std::string reason = wanted.isEmpty() ? "no JACK server (PipeWire's JACK) lists a device"
                                                : "JACK lists no device " + device;
    return reason + "; using " + kept.toStdString() + ", whose audio thread runs without real-time priority";
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
                   static_cast<std::size_t>(std::max(numSamples, 0)), hostSeconds(), outputs_);
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
      buffers_(static_cast<std::size_t>(outputs),
               std::vector<float>(static_cast<std::size_t>(e.info().blockSize))),
      thread_([this](const espresso::stop_token& stop) { run(stop); }) {}

void timerDevice::run(const espresso::stop_token& stop) {
    using clock = std::chrono::steady_clock;
    const std::vector<std::span<float>> outs(buffers_.begin(), buffers_.end());
    const int n = engine_.info().blockSize;
    const auto period = std::chrono::duration<double>(n / engine_.info().sampleRate);
    // A device's audio thread is real-time; the timer that stands in for
    // it asks for the same, or a preempted block counts as a missed deadline.
    if (const std::string error = makeCurrentThreadRealtimeFor(engine_.info().sampleRate, n);
        !error.empty()) {
        std::cerr << "espresso-server: the no-audio timer runs without real-time priority (" << error
                  << "); a missed deadline may then be the timer preempted\n";
    }
    auto next = clock::now();
    while (!stop.stop_requested()) {
        engine_.process({.inputs = {}, .outputs = outs}, n, hostSeconds());
        next += std::chrono::duration_cast<clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

} // namespace server
