/*
 * File:        vbi_tbc_writer.cpp
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Writes the VBI lines of a CVBS frame stream as a reduced
 *              ld-decode style TBC file for videodat_decoder.py
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vbi_tbc_writer.h"

#include <algorithm>

namespace orc::plugins::videodat {

VbiTbcWriter::~VbiTbcWriter()
{
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

bool VbiTbcWriter::open(const std::string& path, size_t width)
{
    error_.clear();
    path_ = path;
    width_ = width;
    fields_written_ = 0;
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) {
        error_ = "cannot create " + path;
        return false;
    }
    buffer_.assign(kTbcFieldLines * width_, 0);
    return true;
}

bool VbiTbcWriter::write_field(const VideoFrameRepresentation& source, FrameID frame,
                               size_t first_frame_line, size_t frame_height, bool yc)
{
    std::fill(buffer_.begin(), buffer_.end(), uint16_t{0});
    for (size_t k = 0; k < kTbcFieldLines; ++k) {
        const size_t frame_line = first_frame_line + k;
        if (frame_line >= frame_height) {
            break;
        }
        uint16_t* out = buffer_.data() + k * width_;
        if (yc) {
            // Y/C sources: VideoDat is a luma signal.
            const auto* line = source.get_line_luma(frame, frame_line);
            if (line == nullptr) {
                continue;
            }
            for (size_t i = 0; i < width_; ++i) {
                out[i] = cvbs_to_tbc_sample(line[i]);
            }
        } else {
            // get_line_samples() lets the source read single lines instead of
            // whole frames, which is most of the cost of this pass.
            const auto line = source.get_line_samples(frame, frame_line);
            const size_t n = std::min(width_, line.size());
            for (size_t i = 0; i < n; ++i) {
                out[i] = cvbs_to_tbc_sample(line[i]);
            }
        }
    }
    // The TBC format is little-endian; all supported hosts are too.
    const size_t n = buffer_.size();
    if (std::fwrite(buffer_.data(), sizeof(uint16_t), n, file_) != n) {
        error_ = "write error on " + path_;
        return false;
    }
    ++fields_written_;
    return true;
}

bool VbiTbcWriter::write_frame(const VideoFrameRepresentation& source, FrameID frame)
{
    if (file_ == nullptr) {
        error_ = "file not open";
        return false;
    }
    const auto params = source.get_video_parameters();
    if (!params || !params->is_valid()) {
        error_ = "input carries no video parameters";
        return false;
    }
    const size_t height = static_cast<size_t>(params->frame_height);
    const size_t f1 = field1_lines(params->system);
    const bool yc = source.has_separate_channels();
    return write_field(source, frame, 0, height, yc) &&
           write_field(source, frame, f1, height, yc);
}

bool VbiTbcWriter::close()
{
    if (file_ == nullptr) {
        return error_.empty();
    }
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    if (!ok) {
        error_ = "write error on " + path_;
        return false;
    }

    // Field order for the decoder (tbc_field_order() reads isFirstField):
    // the stage always writes the first field of each frame first.
    std::FILE* js = std::fopen((path_ + ".json").c_str(), "wb");
    if (js == nullptr) {
        error_ = "cannot create " + path_ + ".json";
        return false;
    }
    std::fputs("{\"fields\": [", js);
    for (size_t i = 0; i < fields_written_; ++i) {
        std::fputs(i == 0 ? "" : ", ", js);
        std::fputs(i % 2 == 0 ? "{\"isFirstField\": true}" : "{\"isFirstField\": false}", js);
    }
    std::fputs("]}\n", js);
    if (std::fclose(js) != 0) {
        error_ = "write error on " + path_ + ".json";
        return false;
    }
    return true;
}

} // namespace orc::plugins::videodat
