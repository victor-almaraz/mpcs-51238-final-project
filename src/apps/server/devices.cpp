#include "devices.h"

#include <chrono>
#include <cstring>
#include <iostream>

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

void audioCallback::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&) {
    engine_.process({in, numIn, out, numOut}, numSamples, hostSeconds());
}

void midiInputs::rescan() {
    for (const auto& d : juce::MidiInput::getAvailableDevices()) {
        if (open_.count(d.identifier)) continue;
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
    std::memcpy(p.bytes.data(), m.getRawData(), static_cast<std::size_t>(size));
    io_.deliverMidi(source->getName().toStdString(), p);
}

void midiOutputs::send(std::string_view pattern, const primitives::midiPacket& p) {
    auto& slot = open_[std::string(pattern)];
    if (!slot) {
        for (const auto& d : juce::MidiOutput::getAvailableDevices()) {
            if (primitives::ioHub::matches(pattern, d.name.toStdString())) {
                slot = juce::MidiOutput::openDevice(d.identifier);
                break;
            }
        }
    }
    if (slot) slot->sendMessageNow(juce::MidiMessage(p.bytes.data(), p.size));
}

timerDevice::timerDevice(primitives::engine& e, int outputs)
    : engine_(e),
      buffers_(static_cast<std::size_t>(outputs), std::vector<float>(static_cast<std::size_t>(e.info().blockSize))),
      thread_([this] { run(); }) {}

timerDevice::~timerDevice() {
    stop_ = true;
    thread_.join();
}

void timerDevice::run() {
    using clock = std::chrono::steady_clock;
    std::vector<float*> outs;
    for (auto& b : buffers_) outs.push_back(b.data());
    const int n = engine_.info().blockSize;
    const auto period = std::chrono::duration<double>(n / engine_.info().sampleRate);
    auto next = clock::now();
    while (!stop_) {
        engine_.process({nullptr, 0, outs.data(), static_cast<int>(outs.size())}, n, hostSeconds());
        next += std::chrono::duration_cast<clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

} // namespace server
