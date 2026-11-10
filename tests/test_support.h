// Shared helpers for the VideoDat plugin tests.
#pragma once

#include <orc/abi/orc_plugin_sdk.h>

#include <iostream>
#include <string>
#include <vector>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond \
                      << '\n';                                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace videodat_test {

// In-memory PAL composite source: every sample of frame f, frame line l is
// (f * 7 + l) % 1024, so tests can tell lines and frames apart.
class SyntheticPal : public orc::Artifact, public orc::VideoFrameRepresentation {
public:
    explicit SyntheticPal(size_t frames)
        : orc::Artifact(orc::ArtifactID("synthetic"), orc::Provenance{})
    {
        params_.system = orc::VideoSystem::PAL;
        params_.frame_width_nominal = 1135;
        params_.frame_height = 625;
        const size_t total = orc::frame_line_sample_offset(orc::VideoSystem::PAL, 1135, 624) +
                             orc::frame_line_sample_count(orc::VideoSystem::PAL, 1135, 624);
        for (size_t f = 0; f < frames; ++f) {
            std::vector<int16_t> frame(total);
            for (size_t l = 0; l < 625; ++l) {
                const size_t off = orc::frame_line_sample_offset(orc::VideoSystem::PAL, 1135, l);
                const size_t n = orc::frame_line_sample_count(orc::VideoSystem::PAL, 1135, l);
                for (size_t i = 0; i < n; ++i) {
                    frame[off + i] = static_cast<int16_t>(value(f, l));
                }
            }
            frames_.push_back(std::move(frame));
        }
    }

    static int value(size_t frame, size_t line) { return static_cast<int>((frame * 7 + line) % 1024); }

    std::string type_name() const override { return "SyntheticPal"; }

    orc::FrameIDRange frame_range() const override
    {
        return {0, frames_.empty() ? 0 : frames_.size() - 1};
    }
    size_t frame_count() const override { return frames_.size(); }
    bool has_frame(orc::FrameID id) const override { return id < frames_.size(); }
    std::optional<orc::FrameDescriptor> get_frame_descriptor(orc::FrameID id) const override
    {
        if (!has_frame(id)) return std::nullopt;
        orc::FrameDescriptor d;
        d.frame_id = id;
        d.system = orc::VideoSystem::PAL;
        d.height = 625;
        d.samples_total = frames_[id].size();
        d.samples_per_line_nominal = 1135;
        return d;
    }
    const sample_type* get_frame(orc::FrameID id) const override
    {
        return has_frame(id) ? frames_[id].data() : nullptr;
    }
    std::vector<sample_type> get_frame_copy(orc::FrameID id) const override
    {
        return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
    }
    std::optional<orc::SourceParameters> get_video_parameters() const override { return params_; }

private:
    orc::SourceParameters params_{};
    std::vector<std::vector<int16_t>> frames_;
};

} // namespace videodat_test
