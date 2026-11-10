#include "vbi_tbc_writer.h"
#include "test_support.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using orc::plugins::videodat::cvbs_to_tbc_sample;
using orc::plugins::videodat::kTbcFieldLines;
using orc::plugins::videodat::VbiTbcWriter;
using videodat_test::SyntheticPal;

int main(int argc, char** argv)
{
    CHECK(cvbs_to_tbc_sample(-5) == 0);
    CHECK(cvbs_to_tbc_sample(256) == 256 * 64);
    CHECK(cvbs_to_tbc_sample(2000) == 1023 * 64);

    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string path = dir + "/writer_test.tbc";
    SyntheticPal source(3);

    VbiTbcWriter w;
    CHECK(w.open(path, 1135));
    for (orc::FrameID f = 0; f < 3; ++f) {
        CHECK(w.write_frame(source, f));
    }
    CHECK(w.close());
    CHECK(w.fields_written() == 6);

    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() == 6 * kTbcFieldLines * 1135 * 2);
    const auto* s = reinterpret_cast<const uint16_t*>(bytes.data());
    for (size_t field = 0; field < 6; ++field) {
        const size_t frame = field / 2;
        const size_t first_line = (field % 2 == 0) ? 0 : 313;  // field 2 starts at frame line 314
        for (size_t k = 0; k < kTbcFieldLines; ++k) {
            const uint16_t* line = s + (field * kTbcFieldLines + k) * 1135;
            const uint16_t want = cvbs_to_tbc_sample(
                static_cast<int16_t>(SyntheticPal::value(frame, first_line + k)));
            CHECK(line[0] == want);
            CHECK(line[1134] == want);
        }
    }

    std::ifstream js(path + ".json");
    std::string text((std::istreambuf_iterator<char>(js)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"isFirstField\": true") != std::string::npos);
    CHECK(text.find("\"isFirstField\": false") != std::string::npos);

    std::filesystem::remove(path);
    std::filesystem::remove(path + ".json");
    std::cout << "writer test passed\n";
    return 0;
}
