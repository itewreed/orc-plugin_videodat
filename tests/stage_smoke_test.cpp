#include "videodat_sink_stage.h"
#include "plugin.h"
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

using orc::plugins::videodat::DecoderOptions;
using orc::plugins::videodat::VideoDatSinkStage;

namespace {

const orc::ParameterDescriptor* find(const std::vector<orc::ParameterDescriptor>& d,
                                     const std::string& name)
{
    for (const auto& p : d) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}


bool throws(const std::map<std::string, orc::ParameterValue>& p)
{
    try {
        VideoDatSinkStage::parse_options(p);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    VideoDatSinkStage stage;
    const auto info = stage.get_node_type_info();
    CHECK(info.stage_name == "videodat_decoder");
    CHECK(info.type == orc::NodeType::SINK);
    CHECK(info.category() == orc::StageCategory::SINK);
    CHECK(stage.required_input_count() == 1 && stage.output_count() == 0);
    CHECK(!stage.get_preview_capability().is_valid());
    CHECK(stage.get_configuration_status() == orc::ConfigurationStatus::Red);

    const auto d = stage.get_parameter_descriptors(orc::VideoSystem::PAL, orc::SourceType::Unknown);

    // Plain options are checkboxes.
    for (const char* name : {"no_sync", "no_eq", "partial", "skip_services", "dump_other",
                             "decrypt_services", "decrypt_files", "save_bytes"}) {
        const auto* p = find(d, name);
        CHECK(p != nullptr);
        CHECK(p->type == orc::ParameterType::BOOL);
    }
    // Options of the 1993 format are greyed out (not hidden) for 1990.
    for (const char* name : {"no_eq", "dump_other", "decrypt_services", "decrypt_files"}) {
        const auto& dep = find(d, name)->constraints.depends_on;
        CHECK(dep.has_value());
        CHECK(dep->parameter_name == "variant");
        CHECK(!dep->hide_when_disabled);
        CHECK(has(dep->required_values, "1993") && has(dep->required_values, "auto"));
        CHECK(!has(dep->required_values, "1990"));
    }
    // Dependencies must not form cycles (the host GUI cannot resolve them).
    for (const auto& p : d) {
        if (!p.constraints.depends_on) continue;
        const auto* parent = find(d, p.constraints.depends_on->parameter_name);
        CHECK(parent != nullptr);
        CHECK(!parent->constraints.depends_on.has_value());
    }
    // Dropdown, integer, path fields.
    CHECK(find(d, "variant")->constraints.allowed_strings.size() == 3);
    CHECK(find(d, "scan_seconds")->type == orc::ParameterType::INT32);
    CHECK(find(d, "output_path")->type == orc::ParameterType::FILE_PATH);
    CHECK(find(d, "output_path")->output_path);
    CHECK(find(d, "save_bytes_path")->type == orc::ParameterType::FILE_PATH);
    CHECK(find(d, "save_bytes_path")->constraints.depends_on->parameter_name == "save_bytes");
    // Path parameters must contain "_path" so the host resolves them against
    // the project directory.
    for (const auto& p : d) {
        if (p.type == orc::ParameterType::FILE_PATH) {
            CHECK(p.name.find("_path") != std::string::npos);
        }
    }

    stage.set_parameters({{"output_path", std::string("/tmp/x")}});
    CHECK(stage.get_configuration_status() == orc::ConfigurationStatus::Green);

    // Option parsing and the decoder options.
    std::map<std::string, orc::ParameterValue> p = {
        {"output_path", std::string("/data/tape006")},
        {"variant", std::string("1993")},
        {"lines", std::string(" 11, 12,13 324 ")},
        {"scan_seconds", int32_t{12}},
        {"no_eq", true},
        {"partial", true},
        {"dump_other", true},
        {"decrypt_files", true},
        {"save_bytes", true},
    };
    DecoderOptions o = VideoDatSinkStage::parse_options(p);
    CHECK(o.lines == "11,12,13,324");
    auto a = o.decode_options();
    CHECK(a.outdir == "/data/tape006");
    CHECK(a.variant == "1993");
    CHECK((a.lines == std::vector<int>{11, 12, 13, 324}));
    CHECK(a.scan_seconds == 12);
    CHECK(a.no_eq && a.partial && a.dump_other && a.decrypt_files);
    CHECK(!a.no_sync && !a.keine_dienste && !a.decrypt_services);
    CHECK(a.save_bytes == (std::filesystem::path("/data/tape006") / "tape006.bin").string());

    // 1990: the 1993-only options are dropped even if still ticked.
    p["variant"] = std::string("1990");
    a = VideoDatSinkStage::parse_options(p).decode_options();
    CHECK(!a.no_eq && !a.dump_other && !a.decrypt_files);
    CHECK(a.partial);

    // Explicit byte stream file; a directory gets the default name inside it;
    // no extension gets .bin; save_bytes off drops it.
    p["save_bytes_path"] = std::string("/other/t.bytes");
    CHECK(VideoDatSinkStage::parse_options(p).decode_options().save_bytes == "/other/t.bytes");
    {
        const auto tmp = std::filesystem::temp_directory_path();
        p["save_bytes_path"] = tmp.string();
        CHECK(VideoDatSinkStage::parse_options(p).decode_options().save_bytes == (tmp / "tape006.bin").string());
    }
    p["save_bytes_path"] = std::string("/not/there/");
    CHECK(VideoDatSinkStage::parse_options(p).decode_options().save_bytes ==
          (std::filesystem::path("/not/there/") / "tape006.bin").string());
    p["save_bytes_path"] = std::string("/other/tape006_stream");
    CHECK(VideoDatSinkStage::parse_options(p).decode_options().save_bytes == "/other/tape006_stream.bin");
    p["save_bytes"] = false;
    CHECK(VideoDatSinkStage::parse_options(p).decode_options().save_bytes.empty());

    // Invalid values.
    CHECK(throws({}));
    CHECK(throws({{"output_path", std::string("o")}, {"lines", std::string("11,abc")}}));
    CHECK(throws({{"output_path", std::string("o")}, {"lines", std::string("700")}}));
    CHECK(throws({{"output_path", std::string("o")}, {"variant", std::string("1995")}}));
    CHECK(throws({{"output_path", std::string("o")}, {"scan_seconds", int32_t{0}}}));

    std::cout << "stage smoke test passed\n";
    return 0;
}
