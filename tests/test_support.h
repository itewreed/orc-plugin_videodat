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

namespace videodat_test {

// A PAL composite source whose frame lines 11-13 and 324-326 carry a 1993
// VideoDat byte stream (2 Mbit/s NRZ, 10 bytes per line, start/stop bits,
// bytes inverted), with H-sync on every line. Lines are synthesised on demand.
class SyntheticVideoDat : public orc::Artifact, public orc::VideoFrameRepresentation {
public:
    SyntheticVideoDat(size_t frames, std::vector<uint8_t> stream)
        : orc::Artifact(orc::ArtifactID("synthetic-videodat"), orc::Provenance{}),
          frames_(frames),
          stream_(std::move(stream))
    {
        params_.system = orc::VideoSystem::PAL;
        params_.frame_width_nominal = 1135;
        params_.frame_height = 625;
    }

    std::string type_name() const override { return "SyntheticVideoDat"; }
    orc::FrameIDRange frame_range() const override { return {0, frames_ - 1}; }
    size_t frame_count() const override { return frames_; }
    bool has_frame(orc::FrameID id) const override { return id < frames_; }
    std::optional<orc::FrameDescriptor> get_frame_descriptor(orc::FrameID id) const override
    {
        if (!has_frame(id)) return std::nullopt;
        orc::FrameDescriptor d;
        d.frame_id = id;
        d.system = orc::VideoSystem::PAL;
        d.height = 625;
        d.samples_per_line_nominal = 1135;
        d.samples_total = 1135 * 625 + 4;
        return d;
    }
    const sample_type* get_frame(orc::FrameID id) const override
    {
        if (!has_frame(id)) return nullptr;
        scratch_.assign(1135 * 625 + 4, 0);
        for (size_t l = 0; l < 625; ++l) {
            const auto line = get_line_samples(id, l);
            const size_t off = orc::frame_line_sample_offset(orc::VideoSystem::PAL, 1135, l);
            std::copy(line.begin(), line.end(), scratch_.begin() + static_cast<long>(off));
        }
        return scratch_.data();
    }
    std::vector<sample_type> get_frame_copy(orc::FrameID id) const override
    {
        get_frame(id);
        return scratch_;
    }
    std::optional<orc::SourceParameters> get_video_parameters() const override { return params_; }

    std::vector<sample_type> get_line_samples(orc::FrameID id, size_t line) const override
    {
        std::vector<sample_type> l(1135, kBlank);
        for (size_t i = 0; i < kSyncEnd; ++i) l[i] = kSyncTip;
        // data slot of this line, if any: frame lines 11-13 (index 10-12) and
        // 324-326 (index 323-325)
        long slot = -1;
        if (line >= 10 && line <= 12) slot = static_cast<long>(line - 10);
        if (line >= 323 && line <= 325) slot = 3 + static_cast<long>(line - 323);
        if (slot < 0) return l;
        const size_t first = (static_cast<size_t>(id) * 6 + static_cast<size_t>(slot)) * 10;
        bool bits[100];
        for (int b = 0; b < 10; ++b) {
            const size_t k = first + static_cast<size_t>(b);
            const uint8_t byte = k < stream_.size() ? stream_[k] : 0x00;
            bits[b * 10] = true;  // start bit: bright
            for (int j = 0; j < 8; ++j) bits[b * 10 + 1 + j] = !((byte >> j) & 1);  // inverted
            bits[b * 10 + 9] = false;  // stop bit: dark
        }
        const double spb = 17.734475e6 / 2.0e6;
        for (size_t i = kDataStart; i < 1135; ++i) {
            const long bit = static_cast<long>((static_cast<double>(i) - kDataStart) / spb);
            if (bit >= 100) break;
            l[i] = bits[bit] ? kBright : kBlank;
        }
        return l;
    }

    static constexpr int16_t kSyncTip = 4;
    static constexpr int16_t kBlank = 256;
    static constexpr int16_t kBright = 700;
    static constexpr size_t kSyncEnd = 83;
    static constexpr size_t kDataStart = 200;

private:
    size_t frames_;
    std::vector<uint8_t> stream_;
    orc::SourceParameters params_{};
    mutable std::vector<int16_t> scratch_;
};

// One 1993 packet: payload of 2^n bytes, no permutation (D=E=F=0), key 0.
inline std::vector<uint8_t> make_packet(uint8_t A, uint8_t sub, uint8_t C, const std::vector<uint8_t>& payload)
{
    int n = 0;
    while ((size_t{1} << n) < payload.size()) ++n;
    std::vector<uint8_t> pay = payload;
    pay.resize(size_t{1} << n, 0);
    const uint8_t B = static_cast<uint8_t>(n | (sub << 7));
    std::vector<uint8_t> p = {0x56, 0xAE, A, B, C, 0, 0, 0, 0x58};
    unsigned sum = A + B + C;
    for (uint8_t b : pay) {
        p.push_back(b);
        sum += b;
    }
    p.push_back(static_cast<uint8_t>(sum & 0xFF));
    p.push_back(static_cast<uint8_t>((sum >> 8) & 0xFF));
    return p;
}

} // namespace videodat_test
