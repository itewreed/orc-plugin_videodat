// Runs the whole trigger with a stand-in for the decoder: "python" is sh and
// the "script" records its arguments and the size of the TBC file it gets.
#include "videodat_sink_stage.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using orc::plugins::videodat::VideoDatSinkStage;

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

std::string slurp(const fs::path& p)
{
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    std::cout << "trigger test skipped on Windows\n";
    return 0;
#else
    const fs::path base = fs::absolute(argc > 1 ? argv[1] : ".") / "trigger_test";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path script = base / "fake_decoder.sh";
    {
        std::ofstream s(script);
        s << "#!/bin/sh\n"
             "# sh -u <script> <tbc> ...: $1 is the TBC file\n"
             "out=$(dirname \"$0\")/args.txt\n"
             "echo \"$@\" > \"$out\"\n"
             "wc -c < \"$1\" > \"$(dirname \"$0\")/size.txt\"\n"
             "echo 'Variante 1993'\n"
             "echo '2 Dateien, davon 1 vollstaendig'\n"
             "exit 0\n";
    }
    // sh ignores -u's argument semantics: "sh -u script" runs the script
    // with unset-variable checking, which the script does not trip.
    auto source = std::make_shared<videodat_test::SyntheticPal>(4);
    const fs::path out = base / "out";
    std::map<std::string, orc::ParameterValue> params = {
        {"output_path", out.string()},
        {"python", std::string("sh")},
        {"decoder_script_path", script.string()},
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
    CHECK(stage.get_trigger_status().find("2 Dateien, davon 1 vollstaendig") != std::string::npos);

    const std::string args = slurp(base / "args.txt");
    CHECK(args.find("--format tbc") != std::string::npos);
    CHECK(args.find("--tbc-field-lines 40") != std::string::npos);
    CHECK(args.find("--save-bytes " + (out / "out.bytes").string()) != std::string::npos);
    // 4 frames = 8 fields of 40 lines x 1135 samples x 2 bytes.
    CHECK(std::stol(slurp(base / "size.txt")) == 8L * 40 * 1135 * 2);
    // The intermediate file is gone, the log is there.
    CHECK(!fs::exists(out / ".videodat_vbi.tbc"));
    CHECK(!fs::exists(out / ".videodat_vbi.tbc.json"));
    CHECK(slurp(out / "videodat_decoder.log").find("Variante 1993") != std::string::npos);

    // A failing decoder is reported with its last line.
    {
        std::ofstream s(script);
        s << "#!/bin/sh\necho 'ModuleNotFoundError: numpy'\nexit 1\n";
    }
    CHECK(!stage.trigger({source}, params, ctx));
    CHECK(stage.get_trigger_status().find("numpy") != std::string::npos);
    CHECK(!fs::exists(out / ".videodat_vbi.tbc"));

    // Missing interpreter.
    params["python"] = std::string("no-such-python-xyz");
    CHECK(!stage.trigger({source}, params, ctx));
    std::cout << stage.get_trigger_status() << '\n';

    // Missing script.
    params["python"] = std::string("sh");
    params["decoder_script_path"] = (base / "missing.py").string();
    CHECK(!stage.trigger({source}, params, ctx));
    CHECK(stage.get_trigger_status().find("not found") != std::string::npos);

    fs::remove_all(base);
    std::cout << "trigger test passed\n";
    return 0;
#endif
}
