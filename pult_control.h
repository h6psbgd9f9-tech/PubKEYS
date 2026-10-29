#pragma once
#include "common.h"
#include <filesystem>
#include <map>

// Local GUI transport only. Search configuration is applied by the owning
// thread after every old worker and monitor has joined.
namespace pult {
struct Settings { Config config; uint64_t revision = 0; };
struct Command {
    uint64_t revision = 0;
    std::string action;
    int power = 0;
    uint64_t settings_revision = 0;
};
bool parse_settings_text(const std::string&, const Config&, Settings&, std::string&);
bool parse_command_text(const std::string&, Command&, std::string&);
bool command_is_new(uint64_t revision, uint64_t last);
bool settings_revision_matches(uint64_t actual, uint64_t expected, uint64_t snapshot);
// Compare validated GUI settings without mutating running search state. The
// requested snapshot preserves automatic choices; effective paths are those
// selected by shared startup preparation. Target file identity is checked by
// the owning application in addition to this value comparison.
bool same_editable_settings(const Config& candidate, const Config& accepted_request,
                            const Config& effective, int current_power, bool gpu_active);

class Control {
public:
    bool open(const std::string& directory, const std::string& executable_directory,
              const Config& defaults, std::string& error);
    bool next_command(Command&, std::string& error);
    bool read_settings(uint64_t revision, const Config& defaults, Settings&, std::string&);
    void acknowledge(uint64_t revision) { last_command_ = revision; }
    void acknowledge_noop(uint64_t revision) { noop_revision_ = revision; }
    void reject_settings(uint64_t revision) { rejected_revision_ = revision; }
    void applied(const Settings& settings);
    void status(const std::string& phase, uint64_t count, uint64_t found, int power,
                double rate, const std::string& sample = {}, const std::string& error = {},
                const std::string& stage = {}, const std::string& detail = {});
    bool initial_sample(const std::map<std::string, std::string>& fields);
    // Export only whitelisted effective search settings. This is a private
    // resume recipe, never notification credentials or derived sample/found
    // data. Readers must pair its generation and revision with status.ini.
    bool export_resume_settings(const Config&, const std::string& checkpoint_identity = {},
                                const std::string& checkpoint_path = {});
    void invalidate_resume() noexcept { resume_available_ = false; }
    uint64_t actual_revision() const { return settings_.revision; }
    const Config& config() const { return settings_.config; }
    const Settings& settings() const { return settings_; }
private:
    bool write_atomic(const std::string& name, const std::string& text);
    std::filesystem::path directory_;
    Settings settings_;
    uint64_t last_command_ = 0;
    uint64_t noop_revision_ = 0, rejected_revision_ = 0;
    uint64_t resume_generation_ = 0;
    bool resume_available_ = false;
    bool initial_sample_ready_ = false;
    std::string current_sample_;
    std::chrono::steady_clock::time_point started_{};
};
}
