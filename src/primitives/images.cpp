// Image generators and the visual layer (primitives.md §7.7, §7.3). They
// run on the render thread, once per frame: a generator starts from its
// input frame, or a transparent one, and draws over it into its output
// (§5.5.4).

#include <array>
#include <memory>
#include <span>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>

#include "node.h"

namespace primitives {

namespace {

// Starts @p out's frame from @p in, or from a transparent frame.
void startFrom(vPipe& out, const std::shared_ptr<const vPipe>& in) noexcept PROJ_NONBLOCKING {
    if (in) {
        out.image().copyFrom(in->image());
    } else {
        out.image().clear();
    }
}

// The input frame with a color composited over it: an opaque color
// replaces it, a translucent one fades it.
class background final : public nodeBase {
public:
    explicit background(nodeContext& c)
        : nodeBase(c), in_(c.videoIn("in")), out_(c.videoOut("out")),
          color_(c.color("color").value_or(metadata::rgba{0.0, 0.0, 0.0, 1.0})) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        out_->image().fill(color_);
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    metadata::rgba color_;
};

// The visual layer (v1 has one): the server shows its input frame. It
// draws nothing; the plan records the frame it shows.
class videoOut final : public nodeBase {
public:
    explicit videoOut(nodeContext& c) : nodeBase(c) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {}
};

} // namespace

std::span<const factoryEntry> imageFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "background", .make = make<background>},
        {.type = "videoOut", .make = make<videoOut>},
    });
    return factories;
}

} // namespace primitives
