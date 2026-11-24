// What the examples' pages send, for tests that play the examples as their
// pages would (io.md §12, §15.1): each control's value as the page starts
// it, and the rack's cables, delivered as the page bridge delivers them.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/io.h>
#include <espresso/protocol/osc.h>

#include "support/unwrap.h"

namespace support {

/// A control's address and its value.
using pageValue = std::pair<std::string, float>;

/// The messages that set each control of @p values, in order.
inline std::vector<protocol::message> pageMessages(std::span<const pageValue> values) {
    std::vector<protocol::message> out;
    out.reserve(values.size());
    for (const auto& [address, value] : values) out.push_back({.address = address, .args = {value}});
    return out;
}

/// Delivers @p m from a page to @p hub's OSC input port @p port, as the
/// page bridge does: encoded, in one packet.
inline void deliverPage(primitives::ioHub& hub, std::uint16_t port, const protocol::message& m) {
    hub.deliverOsc(port, unwrap(primitives::oscPacket::of(protocol::encode(m))));
}

/// mini-clone's panel values when it connects (examples/mini-clone/ui/index.html).
inline std::vector<pageValue> miniClonePanel() {
    return {{"/power", 1},
            {"/main", 1},
            {"/volume", 0.7f},
            {"/tune", 0},
            {"/glide", 0},
            {"/modmix", 0},
            {"/osc/mod", 0},
            {"/osc3/control", 1},
            {"/osc1/range", 3},
            {"/osc1/wave", 2},
            {"/osc2/range", 3},
            {"/osc2/freq", 0.108f},
            {"/osc2/wave", 2},
            {"/osc3/range", 3},
            {"/osc3/freq", -0.108f},
            {"/osc3/wave", 3},
            {"/mix/osc1", 0.8f},
            {"/mix/osc1/on", 1},
            {"/mix/ext", 0},
            {"/mix/ext/on", 0},
            {"/mix/osc2", 0.6f},
            {"/mix/osc2/on", 1},
            {"/mix/noise", 0.3f},
            {"/mix/noise/on", 0},
            {"/noise/pink", 0},
            {"/mix/osc3", 0.4f},
            {"/mix/osc3/on", 1},
            {"/filter/mod", 0},
            {"/filter/kb1", 1},
            {"/filter/kb2", 0},
            {"/filter/cutoff", 0},
            {"/filter/emphasis", 0.3f},
            {"/filter/amount", 2.5f},
            {"/filter/attack", 0.01f},
            {"/filter/decay", 0.24f},
            {"/filter/sustain", 0.3f},
            {"/amp/attack", 0.01f},
            {"/amp/decay", 0.18f},
            {"/amp/sustain", 0.8f},
            {"/a440", 0},
            {"/glide/on", 1},
            {"/decay/on", 1}};
}

/// The rack's knobs as its page starts them (examples/rack/ui/index.html),
/// in their units.
inline std::vector<pageValue> rackKnobs() {
    return {
        {"/lfo/rate", 2},        {"/env1/attack", 0.005f}, {"/env1/decay", 0.3f}, {"/env1/sustain", 0.6f},
        {"/env1/release", 0.4f}, {"/env2/attack", 0.01f},  {"/env2/decay", 0.4f}, {"/env2/sustain", 0.2f},
        {"/env2/release", 0.3f}, {"/vco1/octave", 0},      {"/vco1/tune", 0},     {"/vco1/fm", 0},
        {"/vco1/width", 0.5f},   {"/vco1/pwm", 0},         {"/vco2/octave", 0},   {"/vco2/tune", 0.07f},
        {"/vco2/fm", 0},         {"/vco2/width", 0.5f},    {"/vco2/pwm", 0},      {"/mix/1", 0.7f},
        {"/mix/2", 0.5f},        {"/mix/3", 0.5f},         {"/vcf/cutoff", 800},  {"/vcf/q", 1.5f},
        {"/vcf/cv", 3},          {"/vca/level", 0},        {"/delay/time", 0.3f}, {"/delay/feedback", 0.35f},
        {"/delay/mix", 0.25f},   {"/out/volume", 0.6f}};
}

/// The address that plugs (1) or pulls (0) the rack's cable from output
/// @p from to input @p to.
inline std::string rackCable(std::string_view from, std::string_view to) {
    return "/cable/" + std::string(from) + "/" + std::string(to);
}

/// The rack's cables for a played key: two oscillators at the keyboard's
/// pitch through the mixer, the filter, the amplifier under the first
/// envelope, and the delay to both outputs, the envelopes gated by the
/// keyboard.
inline std::vector<std::pair<std::string_view, std::string_view>> rackKeyPatch() {
    return {{"kbd-pitch", "vco1-pitch"}, {"kbd-pitch", "vco2-pitch"}, {"vco1-saw", "mix-1"},
            {"vco2-pulse", "mix-2"},     {"mix-out", "vcf-in"},       {"vcf-low", "vca-in"},
            {"env1-out", "vca-cv"},      {"env2-out", "vcf-cv"},      {"kbd-gate", "env1-gate"},
            {"kbd-gate", "env2-gate"},   {"vca-out", "delay-in"},     {"delay-out", "out-l"},
            {"delay-out", "out-r"}};
}

}  // namespace support
