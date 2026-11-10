/*
 * File:        vbi_tbc_writer.h
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Writes the VBI lines of a CVBS frame stream as a reduced
 *              ld-decode style TBC file for videodat_decoder.py
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include <orc/abi/orc_plugin_sdk.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace orc::plugins::videodat {

// The Python decoder reads ld-decode/vhs-decode TBC files: unsigned 16-bit
// little-endian samples, 10-bit value << 6, one field after the other, a
// fixed number of lines per field. It only ever looks at the first 40 lines
// of a field (VideoDat sits in the vertical blanking interval), so the stage
// writes just those, which keeps the intermediate file at roughly 1/8 of a
// full TBC. Field line k of the output is field line k of the input (k = 0 is
// frame line 1 for the first field and frame line 314 for the second), which
// is the numbering the decoder's --lines option and its report use.
inline constexpr size_t kTbcFieldLines = 40;

// Converts one CVBS_U10_4FSC sample to the 16-bit TBC domain.
inline uint16_t cvbs_to_tbc_sample(int16_t v)
{
    if (v < 0) {
        v = 0;
    } else if (v > 1023) {
        v = 1023;
    }
    return static_cast<uint16_t>(static_cast<uint16_t>(v) << 6);
}

class VbiTbcWriter {
public:
    VbiTbcWriter() = default;
    ~VbiTbcWriter();
    VbiTbcWriter(const VbiTbcWriter&) = delete;
    VbiTbcWriter& operator=(const VbiTbcWriter&) = delete;

    // Opens |path| for writing. |width| is the samples per line written for
    // every line (the nominal PAL width, 1135).
    bool open(const std::string& path, size_t width);

    // Appends both fields of |frame| from |source|. Returns false on a read or
    // write error; error() then says which.
    bool write_frame(const VideoFrameRepresentation& source, FrameID frame);

    // Closes the file and writes |path|.json next to it (field order, so the
    // decoder does not have to guess it).
    bool close();

    size_t fields_written() const { return fields_written_; }
    const std::string& error() const { return error_; }

private:
    bool write_field(const VideoFrameRepresentation& source, FrameID frame,
                     size_t first_frame_line, size_t frame_height, bool yc);

    std::FILE* file_{nullptr};
    std::string path_;
    size_t width_{0};
    size_t fields_written_{0};
    std::vector<uint16_t> buffer_;
    std::string error_;
};

} // namespace orc::plugins::videodat
