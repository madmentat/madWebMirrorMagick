#pragma once
#include <string>
#include <cstdint>
#include <filesystem>
#include <chrono>
#include <vector>

namespace mad {
namespace fs = std::filesystem;
using clock_ = std::chrono::steady_clock;



struct Config; // реализация в core.cpp

// ───────── Сущности новой архитектуры (multi-site / multi-mirror / proxy) ─────────
struct Site {
    std::string name;                 // "site1"
    std::string server_name;          // домен
    std::string local_site_dir;
    std::string db_user, db_pass, db_name;
    std::vector<std::string> mirrors; // имена зеркал в порядке приоритета
};

struct Mirror {
    std::string name;                 // "mirror1"
    std::string remote_host;
    int ssh_port = 22;
    std::string remote_user, remote_pass, remote_sudo_pass, remote_root_pass;
    std::string target_server;        // "nginx" | "apache2"
    std::string remote_site_dir;
    std::string remote_backup_base;
    int local_http_port = 8081;
    int local_https_port = 0;
    std::string php_version, php_fpm_sock;
    std::string ssl_cert, ssl_key;
    std::string health_url;           // как proxy/демон видят копию на этом зеркале
    int fail_threshold = 3;           // N подряд неудач → DOWN
    int ok_threshold = 3;             // N подряд успехов → UP
};

struct Proxy {
    std::string host;
    int ssh_port = 22;
    std::string user, pass, sudo_pass;
    std::string server_type;          // "nginx" | "apache2"
    std::string health_url;           // как демон видит proxy
    int fail_threshold = 3;
    int ok_threshold = 3;
};


// utils
std::string base64_encode(const std::string& in);
std::string trim(const std::string& s);
std::string today();
std::string human_size(uint64_t bytes);
uint64_t dir_size_bytes(const std::filesystem::path& root);
bool has_command(const char* name);
int  run_local(const std::string& cmd, bool echo = true);
int  run_with_spinner(const std::string& cmd, const std::string& label);

// config IO & validation
extern const char* CFG_PATH_PRIMARY;
extern const char* CFG_PATH_FALLBACK;
extern const char* DEFAULT_TARGET;
extern const int   DEFAULT_SSH_PORT;
extern const int   DEFAULT_LOCAL_HTTP_PORT;
extern const int   DEFAULT_LOCAL_HTTPS_PORT;
extern const bool  DEFAULT_SWITCH_TO_LOCAL;
extern const std::string DEFAULT_PHP_VERSION;
extern const int   DEFAULT_HEALTH_INTERVAL_SEC; // NEW

struct Config {
    // основные
    std::string target_server; // "nginx" | "apache2"
    std::string remote_host;
    int         ssh_port;
    std::string remote_user;
    std::string remote_pass;
    std::string remote_sudo_pass;
    std::string remote_root_pass;

    // пути
    std::string local_site_dir;
    std::string remote_site_dir;
    std::string remote_backup_base;


    // веб
    std::string server_name;

    // PHP
    std::string php_version;
    std::string php_fpm_sock;

    // БД
    std::string db_user;
    std::string db_pass;
    std::string db_name;

    // прокси/переключение
    std::string proxy_target;
    int         local_http_port;
    int         local_https_port;
    bool        switch_to_local;

     // Health-check
     std::string health_url;          // если пусто → берём http://127.0.0.1:<local_http_port>/
     std::string health_host_header;  // если непусто → добавляем curl -H "Host: <...>"

    // SSL
    std::string ssl_cert;
    std::string ssl_key;

    // флаги
    bool        skip_tar{false};
    bool        skip_sql{false};
    bool        skip_upload{false};

    // демонизация/расписание
    int         health_interval_sec = 60;    // период health-check, сек
    std::string schedule_hhmm = "04:00";     // время ежедневного запуска

    // ── новая архитектура (multi-site / multi-mirror / proxy) ──
    std::string main_health_url;      // main как его видят proxy/зеркала извне (NAT!)
    std::vector<Site> sites;
    std::vector<Mirror> mirrors;
    Proxy proxy;
    bool has_proxy = false;
    bool section_format = false;      // конфиг в секционном формате ([site:...]/[mirror:...])
};

void write_default_config(const std::string& path);
void load_kv_file(const std::string& path, Config& cfg);
void apply_cli_kv(int argc, char** argv, Config& cfg);
bool validate(const Config& c, std::string& err);

} // namespace mad
