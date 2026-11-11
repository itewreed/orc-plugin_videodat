// Runs the whole stage on a synthetic VideoDat signal: one file sent in a
// loop, which the built-in decoder has to find, slice and reassemble.
#include "videodat_sink_stage.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>

namespace fs = std::filesystem;
using orc::plugins::videodat::VideoDatSinkStage;
using videodat_test::make_packet;

namespace {

class NullObservationContext : public orc::IObservationContext {
public:
    void set(orc::FieldID, const std::string&, const std::string&, const orc::ObservationValue&) override {}
    std::optional<orc::ObservationValue> get(orc::FieldID, const std::string&,
                                             const std::string&) const override
    {
        return std::nullopt;
    }
    bool has(orc::FieldID, const std::string&, const std::string&) const override { return false; }
    std::vector<std::string> get_keys(orc::FieldID, const std::string&) const override { return {}; }
    std::vector<std::string> get_namespaces(orc::FieldID) const override { return {}; }
    std::map<std::string, std::map<std::string, orc::ObservationValue>> get_all_observations(
        orc::FieldID) const override
    {
        return {};
    }
    void clear() override {}
    void clear_field(orc::FieldID) override {}
    void register_schema(const std::vector<orc::ObservationKey>&) override {}
    void clear_schema() override {}
};

std::vector<uint8_t> slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string text(const fs::path& p)
{
    const auto b = slurp(p);
    return std::string(b.begin(), b.end());
}

} // namespace

int main(int argc, char** argv)
{
    // The file and its transmission: header, two data blocks, end mark.
    std::vector<uint8_t> content(300);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    const uint32_t dos = (uint32_t{((1993 - 1980) << 9) | (12 << 5) | 16} << 16) | (12u << 11);
    std::vector<uint8_t> stream;
    uint8_t C = 0;
    for (int cycle = 0; cycle < 40; ++cycle) {
        std::vector<uint8_t> head = {0x02, 1, 2, 3, 4};
        for (int i = 0; i < 4; ++i) head.push_back(static_cast<uint8_t>(content.size() >> (8 * i)));
        for (int i = 0; i < 4; ++i) head.push_back(static_cast<uint8_t>(dos >> (8 * i)));
        const std::string name = "TEST.TXT";
        head.push_back(static_cast<uint8_t>(name.size()));
        head.insert(head.end(), name.begin(), name.end());
        auto add = [&](const std::vector<uint8_t>& pay) {
            const auto p = make_packet(0x41, 0, C++, pay);
            stream.insert(stream.end(), p.begin(), p.end());
            stream.insert(stream.end(), 13, 0x00);  // filler between packets
        };
        add(head);
        for (size_t blk = 0; blk < 2; ++blk) {
            std::vector<uint8_t> pay = {0x04};
            for (size_t i = 0; i < 255; ++i) {
                const size_t k = blk * 255 + i;
                pay.push_back(k < content.size() ? content[k] : 0);
            }
            add(pay);
        }
        add({0x03, 1, 2, 3, 4, 0, 0, 0});
    }
    const size_t frames = stream.size() / 60 + 1;
    auto source = std::make_shared<videodat_test::SyntheticVideoDat>(frames, stream);

    const fs::path base = fs::absolute(argc > 1 ? argv[1] : ".") / "trigger_test";
    fs::remove_all(base);
    const fs::path out = base / "tape";
    std::map<std::string, orc::ParameterValue> params = {
        {"output_path", out.string()},
        {"save_bytes", true},
    };

    VideoDatSinkStage stage;
    size_t calls = 0;
    stage.set_progress_callback([&](size_t, size_t, const std::string&) { ++calls; });
    NullObservationContext ctx;
    const bool ok = stage.trigger({source}, params, ctx);
    std::cout << stage.get_trigger_status() << '\n';
    CHECK(ok);
    CHECK(calls > 0);
    CHECK(stage.get_trigger_status().find("1 Dateien, davon 1 vollstaendig") != std::string::npos);
    CHECK(slurp(out / "strom_41" / "TEST.TXT") == content);
    const std::string log = text(out / "videodat_decoder.log");
    CHECK(log.find("VideoDat-Zeilen (automatisch, Bildzeilen PAL): [11, 12, 13, 324, 325, 326]") != std::string::npos);
    CHECK(log.find("Variante 1993") != std::string::npos);
    // The byte stream is the transmitted one.
    const auto bytes = slurp(out / "tape.bin");
    CHECK(bytes.size() >= stream.size());
    CHECK(std::equal(stream.begin(), stream.end(), bytes.begin()));
    CHECK(fs::exists(out / "dienste" / "dienste.txt"));

    // Given lines, no equalizer, 1990 forced: the 1993 options are dropped and
    // nothing decodes, but the run succeeds.
    params["variant"] = std::string("1990");
    params["lines"] = std::string("11,12,13,324,325,326");
    params["decrypt_files"] = true;
    CHECK(stage.trigger({source}, params, ctx));
    CHECK(text(out / "videodat_decoder.log").find("Entzerrer: fuer Variante 1990 nicht verwendet") !=
          std::string::npos);

    // Missing output directory.
    CHECK(!stage.trigger({source}, {}, ctx));
    CHECK(stage.get_trigger_status().find("output directory") != std::string::npos);

    // Cancel before the run: the decoder stops at its first check.
    {
        VideoDatSinkStage s2;
        s2.set_progress_callback([&](size_t, size_t, const std::string&) { s2.cancel_trigger(); });
        params.erase("variant");
        params.erase("lines");
        CHECK(!s2.trigger({source}, params, ctx));
        CHECK(s2.get_trigger_status() == "Cancelled");
    }

    fs::remove_all(base);
    std::cout << "trigger test passed\n";
    return 0;
}
