#include "mad/core.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace mad {

std::string trim(const std::string& s) {
    const auto l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    const auto r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

static std::string format_local_time(const char* format) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64]{};
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

std::string today() {
    return format_local_time("%Y-%m-%d");
}

std::string timestamp() {
    return format_local_time("%Y%m%d-%H%M%S");
}

std::string human_size(std::uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

std::uint64_t dir_size_bytes(const fs::path& root) {
    std::uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code file_ec;
        if (it->is_regular_file(file_ec)) {
            const auto size = it->file_size(file_ec);
            if (!file_ec) total += static_cast<std::uint64_t>(size);
        }
    }
    return total;
}

std::string shell_quote(const std::string& value) {
    std::string out{"'"};
    out.reserve(value.size() + 8);
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

bool has_command(const char* name) {
    const std::string cmd = "command -v " + shell_quote(name) + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

int run_local(const std::string& cmd, bool echo) {
    if (echo) std::cout << "➜ " << cmd << '\n';
    const int rc = std::system(cmd.c_str());
    if (rc != 0) std::cerr << "❌ Команда завершилась с кодом " << rc << '\n';
    return rc;
}

static bool parse_bool(const std::string& value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static int parse_int(const std::string& value, const std::string& key) {
    try {
        std::size_t used = 0;
        const int result = std::stoi(value, &used);
        if (used != value.size()) throw std::invalid_argument("tail");
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error("Некорректное целое значение для " + key + ": " + value);
    }
}

bool parse_hhmm(const std::string& value, int& hh, int& mm) {
    if (value.size() != 5 || value[2] != ':') return false;
    try {
        hh = std::stoi(value.substr(0, 2));
        mm = std::stoi(value.substr(3, 2));
    } catch (...) {
        return false;
    }
    return hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
}

void write_default_config(const std::string& path) {
    const fs::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) throw std::runtime_error("Не удалось создать конфиг: " + path);

    out << R"(# madbackuper.conf
# Пароли допускаются для совместимости, но предпочтительна SSH-аутентификация ключом.
# Файл автоматически создаётся с правами 0600.

target_server=nginx
remote_host=192.168.88.202
ssh_port=22
remote_user=madbackup
remote_pass=
remote_sudo_pass=

local_site_dir=/webserver/madmentat.ru
remote_site_dir=/webserver/madmentat.ru
remote_backup_base=/webserver/.backup

server_name=madmentat.ru
# Если пусто, используется /root/setup_<первая часть server_name>_nginx.sh
switch_script=

php_version=8.3
php_fpm_sock=

db_user=madmentat
db_pass=
db_name=mad

proxy_target=192.168.88.198
local_http_port=8081
local_https_port=0
switch_to_local=true

health_url=http://127.0.0.1:80/
health_host_header=
health_interval_sec=60
health_failures=3
health_recoveries=3
switch_cooldown_sec=60

ssl_cert=
ssl_key=

schedule_hhmm=04:00
)";
    out.close();
    if (::chmod(path.c_str(), 0600) != 0) {
        throw std::runtime_error("Не удалось установить права 0600 на конфиг: " + path);
    }
}

static void assign_config_value(Config& cfg, const std::string& key, const std::string& value) {
    if      (key == "target_server")         cfg.target_server = value;
    else if (key == "remote_host")           cfg.remote_host = value;
    else if (key == "ssh_port")              cfg.ssh_port = parse_int(value, key);
    else if (key == "remote_user")           cfg.remote_user = value;
    else if (key == "remote_pass")           cfg.remote_pass = value;
    else if (key == "remote_sudo_pass")      cfg.remote_sudo_pass = value;
    else if (key == "local_site_dir")        cfg.local_site_dir = value;
    else if (key == "remote_site_dir")       cfg.remote_site_dir = value;
    else if (key == "remote_backup_base")    cfg.remote_backup_base = value;
    else if (key == "server_name")           cfg.server_name = value;
    else if (key == "switch_script")         cfg.switch_script = value;
    else if (key == "php_version")           cfg.php_version = value;
    else if (key == "php_fpm_sock")          cfg.php_fpm_sock = value;
    else if (key == "db_user")               cfg.db_user = value;
    else if (key == "db_pass")               cfg.db_pass = value;
    else if (key == "db_name")               cfg.db_name = value;
    else if (key == "proxy_target")          cfg.proxy_target = value;
    else if (key == "local_http_port")       cfg.local_http_port = parse_int(value, key);
    else if (key == "local_https_port")      cfg.local_https_port = parse_int(value, key);
    else if (key == "switch_to_local")       cfg.switch_to_local = parse_bool(value);
    else if (key == "health_url")            cfg.health_url = value;
    else if (key == "health_host_header")    cfg.health_host_header = value;
    else if (key == "health_interval_sec")   cfg.health_interval_sec = parse_int(value, key);
    else if (key == "health_failures")       cfg.health_failures = parse_int(value, key);
    else if (key == "health_recoveries")     cfg.health_recoveries = parse_int(value, key);
    else if (key == "switch_cooldown_sec")   cfg.switch_cooldown_sec = parse_int(value, key);
    else if (key == "ssl_cert")              cfg.ssl_cert = value;
    else if (key == "ssl_key")               cfg.ssl_key = value;
    else if (key == "schedule_hhmm")         cfg.schedule_hhmm = value;
}

void load_kv_file(const std::string& path, Config& cfg) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Не удалось открыть конфиг: " + path);

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));
        try {
            assign_config_value(cfg, key, value);
        } catch (const std::exception& e) {
            throw std::runtime_error(path + ":" + std::to_string(line_no) + ": " + e.what());
        }
    }
}

void apply_cli_kv(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--skip-tar")    { cfg.skip_tar = true; continue; }
        if (arg == "--skip-sql")    { cfg.skip_sql = true; continue; }
        if (arg == "--skip-upload") { cfg.skip_upload = true; continue; }

        const auto eq = arg.find('=');
        if (arg.rfind("--", 0) != 0 || eq == std::string::npos) continue;

        std::string key = arg.substr(2, eq - 2);
        const std::string value = arg.substr(eq + 1);
        for (char& c : key) if (c == '-') c = '_';
        if (key == "at" || key == "schedule") key = "schedule_hhmm";
        assign_config_value(cfg, key, value);
    }
}

bool validate(const Config& cfg, std::string& err) {
    const auto required = [&](const std::string& value, const char* name) {
        if (value.empty()) {
            err = std::string("Параметр пуст: ") + name;
            return false;
        }
        return true;
    };

    if (cfg.target_server != "nginx" && cfg.target_server != "apache2") {
        err = "target_server должен быть nginx или apache2";
        return false;
    }
    if (!required(cfg.remote_host, "remote_host")) return false;
    if (!required(cfg.remote_user, "remote_user")) return false;
    if (!required(cfg.local_site_dir, "local_site_dir")) return false;
    if (!required(cfg.remote_site_dir, "remote_site_dir")) return false;
    if (!required(cfg.remote_backup_base, "remote_backup_base")) return false;
    if (!required(cfg.server_name, "server_name")) return false;
    if (!required(cfg.db_user, "db_user")) return false;
    if (!required(cfg.db_name, "db_name")) return false;
    if (!required(cfg.proxy_target, "proxy_target")) return false;

    if (cfg.ssh_port <= 0 || cfg.ssh_port > 65535) {
        err = "ssh_port должен быть в диапазоне 1..65535";
        return false;
    }
    if (cfg.local_http_port <= 0 || cfg.local_http_port > 65535) {
        err = "local_http_port должен быть в диапазоне 1..65535";
        return false;
    }
    if (cfg.local_https_port < 0 || cfg.local_https_port > 65535) {
        err = "local_https_port должен быть в диапазоне 0..65535";
        return false;
    }
    if (cfg.health_interval_sec < 5 || cfg.health_failures < 1 || cfg.health_recoveries < 1 || cfg.switch_cooldown_sec < 0) {
        err = "Некорректные параметры health-check";
        return false;
    }
    int hh = 0, mm = 0;
    if (!parse_hhmm(cfg.schedule_hhmm, hh, mm)) {
        err = "schedule_hhmm должен иметь формат HH:MM";
        return false;
    }
    return true;
}

} // namespace mad
