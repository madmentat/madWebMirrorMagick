#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mad {

namespace fs = std::filesystem;

inline constexpr const char* CFG_PATH_PRIMARY = "/etc/madbackuper.conf";
inline constexpr const char* CFG_PATH_FALLBACK = "/root/madbackuper.conf";

struct Config {
    std::string target_server{"nginx"};
    std::string remote_host;
    int ssh_port{22};
    std::string remote_user;
    std::string remote_pass;
    std::string remote_sudo_pass;

    std::string local_site_dir;
    std::string remote_site_dir;
    std::string remote_backup_base;

    std::string server_name;
    std::string switch_script;

    std::string php_version{"8.3"};
    std::string php_fpm_sock;

    std::string db_user;
    std::string db_pass;
    std::string db_name;

    std::string proxy_target;
    int local_http_port{8081};
    int local_https_port{0};
    bool switch_to_local{true};

    std::string health_url;
    std::string health_host_header;
    int health_interval_sec{60};
    int health_failures{3};
    int health_recoveries{3};
    int switch_cooldown_sec{60};

    std::string ssl_cert;
    std::string ssl_key;

    bool skip_tar{false};
    bool skip_sql{false};
    bool skip_upload{false};

    std::string schedule_hhmm{"04:00"};
};

std::string trim(const std::string& s);
std::string today();
std::string timestamp();
std::string human_size(std::uint64_t bytes);
std::uint64_t dir_size_bytes(const fs::path& root);
bool has_command(const char* name);
int run_local(const std::string& cmd, bool echo = true);
std::string shell_quote(const std::string& value);

void write_default_config(const std::string& path);
void save_config(const std::string& path, const Config& cfg);
void load_kv_file(const std::string& path, Config& cfg);
void apply_cli_kv(int argc, char** argv, Config& cfg);
bool validate(const Config& cfg, std::string& err);
bool parse_hhmm(const std::string& value, int& hh, int& mm);

} // namespace mad
