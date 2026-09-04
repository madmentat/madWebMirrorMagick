#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> keep_running{true};

void on_signal(int) {
    keep_running = false;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

int parse_int(const std::string& value, const std::string& key) {
    try {
        std::size_t used = 0;
        const int result = std::stoi(value, &used);
        if (used != value.size()) throw std::invalid_argument("tail");
        return result;
    } catch (...) {
        throw std::runtime_error("Некорректное число для " + key + ": " + value);
    }
}

std::int64_t parse_i64(const std::string& value, std::int64_t fallback = 0) {
    try {
        std::size_t used = 0;
        const auto result = std::stoll(value, &used);
        return used == value.size() ? result : fallback;
    } catch (...) {
        return fallback;
    }
}

bool has_line_break(const std::string& value) {
    return value.find_first_of("\r\n") != std::string::npos;
}

struct AgentConfig {
    std::string health_url;
    std::string health_host_header;
    std::string switch_script;
    std::string state_file{"/var/lib/madwebmirror/failover.state"};
    std::string lock_file{"/run/madwebmirror/watchdog.lock"};
    std::string curl_path{"/usr/bin/curl"};
    int interval_sec{60};
    int failures{3};
    int recoveries{3};
    int cooldown_sec{60};
};

void assign(AgentConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "health_url") cfg.health_url = value;
    else if (key == "health_host_header") cfg.health_host_header = value;
    else if (key == "switch_script") cfg.switch_script = value;
    else if (key == "state_file") cfg.state_file = value;
    else if (key == "lock_file") cfg.lock_file = value;
    else if (key == "curl_path") cfg.curl_path = value;
    else if (key == "interval_sec") cfg.interval_sec = parse_int(value, key);
    else if (key == "failures") cfg.failures = parse_int(value, key);
    else if (key == "recoveries") cfg.recoveries = parse_int(value, key);
    else if (key == "cooldown_sec") cfg.cooldown_sec = parse_int(value, key);
}

AgentConfig load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Не удалось открыть конфиг агента: " + path);

    AgentConfig cfg;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        try {
            assign(cfg, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
        } catch (const std::exception& e) {
            throw std::runtime_error(path + ":" + std::to_string(line_no) + ": " + e.what());
        }
    }
    return cfg;
}

bool validate(const AgentConfig& cfg, std::string& error) {
    if (cfg.health_url.empty() || has_line_break(cfg.health_url)) {
        error = "health_url пуст или содержит перевод строки";
        return false;
    }
    if (has_line_break(cfg.health_host_header)) {
        error = "health_host_header содержит перевод строки";
        return false;
    }
    if (cfg.switch_script.empty() || cfg.switch_script.front() != '/' || has_line_break(cfg.switch_script)) {
        error = "switch_script должен быть абсолютным путём";
        return false;
    }
    if (cfg.state_file.empty() || cfg.state_file.front() != '/' || has_line_break(cfg.state_file)) {
        error = "state_file должен быть абсолютным путём";
        return false;
    }
    if (cfg.lock_file.empty() || cfg.lock_file.front() != '/' || has_line_break(cfg.lock_file)) {
        error = "lock_file должен быть абсолютным путём";
        return false;
    }
    if (cfg.curl_path.empty() || cfg.curl_path.front() != '/' || has_line_break(cfg.curl_path)) {
        error = "curl_path должен быть абсолютным путём";
        return false;
    }
    if (cfg.interval_sec < 1 || cfg.failures < 1 || cfg.recoveries < 1 || cfg.cooldown_sec < 0) {
        error = "Некорректные интервалы или пороги watchdog";
        return false;
    }
    return true;
}

int run_process(const std::string& executable, std::vector<std::string> args, bool quiet) {
    args.insert(args.begin(), executable);
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (quiet) {
            const int null_fd = ::open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) ::close(null_fd);
            }
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        _exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

bool check_primary(const AgentConfig& cfg) {
    std::vector<std::string> args{
        "--fail", "--silent", "--show-error", "--location",
        "--max-time", "5"
    };
    if (!cfg.health_host_header.empty()) {
        args.push_back("--header");
        args.push_back("Host: " + cfg.health_host_header);
    }
    args.push_back(cfg.health_url);
    return run_process(cfg.curl_path, std::move(args), true) == 0;
}

enum class Route {
    Unknown,
    Primary,
    Backup
};

enum class Decision {
    None,
    UsePrimary,
    UseBackup
};

const char* route_name(Route route) {
    switch (route) {
        case Route::Primary: return "primary";
        case Route::Backup: return "backup";
        case Route::Unknown: return "unknown";
    }
    return "unknown";
}

Route parse_route(const std::string& value) {
    if (value == "primary") return Route::Primary;
    if (value == "backup") return Route::Backup;
    return Route::Unknown;
}

struct RuntimeState {
    Route route{Route::Unknown};
    std::int64_t last_switch{0};
    std::int64_t last_check{0};
    bool primary_healthy{false};
    int failures{0};
    int recoveries{0};

    // Состояние на диске говорит, что было применено раньше, но после запуска
    // агент один раз повторно применяет выбранный маршрут. Это устраняет
    // рассогласование после ручной правки nginx или аварийного рестарта.
    bool reconciled{false};
};

RuntimeState load_state(const std::string& path) {
    RuntimeState state;
    std::ifstream in(path);
    if (!in) return state;

    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "mode") state.route = parse_route(value);
        else if (key == "last_switch") state.last_switch = parse_i64(value);
        else if (key == "last_check") state.last_check = parse_i64(value);
        else if (key == "primary_healthy") state.primary_healthy = value == "1" || value == "true";
    }
    state.reconciled = false;
    return state;
}

bool save_state(const std::string& path, const RuntimeState& state, std::string& error) {
    const fs::path target(path);
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    if (ec) {
        error = "Не удалось создать каталог state: " + ec.message();
        return false;
    }

    const fs::path tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            error = "Не удалось создать " + tmp.string();
            return false;
        }
        out << "version=1\n"
            << "mode=" << route_name(state.route) << '\n'
            << "last_switch=" << state.last_switch << '\n'
            << "last_check=" << state.last_check << '\n'
            << "primary_healthy=" << (state.primary_healthy ? "true" : "false") << '\n';
        if (!out) {
            error = "Ошибка записи " + tmp.string();
            return false;
        }
    }
    if (::chmod(tmp.c_str(), 0600) != 0) {
        fs::remove(tmp);
        error = "Не удалось установить права 0600 для state";
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp);
        error = "Не удалось заменить state: " + ec.message();
        return false;
    }
    return true;
}

Decision observe(RuntimeState& state, const AgentConfig& cfg, bool healthy, std::int64_t now) {
    state.last_check = now;
    state.primary_healthy = healthy;

    if (healthy) {
        state.failures = 0;
        ++state.recoveries;
        if (state.recoveries < cfg.recoveries) return Decision::None;
        if (!state.reconciled) return Decision::UsePrimary;
        const std::int64_t elapsed = now >= state.last_switch ? now - state.last_switch : 0;
        if (state.route != Route::Primary && elapsed >= cfg.cooldown_sec) return Decision::UsePrimary;
        return Decision::None;
    }

    state.recoveries = 0;
    ++state.failures;
    if (state.failures < cfg.failures) return Decision::None;
    if (!state.reconciled) return Decision::UseBackup;
    const std::int64_t elapsed = now >= state.last_switch ? now - state.last_switch : 0;
    if (state.route != Route::Backup && elapsed >= cfg.cooldown_sec) return Decision::UseBackup;
    return Decision::None;
}

int apply_decision(const AgentConfig& cfg, Decision decision) {
    if (decision == Decision::None) return 0;
    const char* argument = decision == Decision::UsePrimary ? "remote" : "local";
    return run_process(cfg.switch_script, {argument}, false);
}

std::int64_t epoch_now() {
    return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

int run_watchdog(const AgentConfig& cfg, bool once) {
    if (::access(cfg.curl_path.c_str(), X_OK) != 0) {
        std::cerr << "❌ curl недоступен: " << cfg.curl_path << '\n';
        return 1;
    }
    if (::access(cfg.switch_script.c_str(), X_OK) != 0) {
        std::cerr << "❌ switch script недоступен: " << cfg.switch_script << '\n';
        return 1;
    }

    const fs::path lock_path(cfg.lock_file);
    std::error_code lock_ec;
    if (lock_path.has_parent_path()) fs::create_directories(lock_path.parent_path(), lock_ec);
    if (lock_ec) {
        std::cerr << "❌ Не удалось создать каталог lock-файла: " << lock_ec.message() << '\n';
        return 1;
    }
    const int lock_fd = ::open(cfg.lock_file.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (lock_fd >= 0) ::close(lock_fd);
        std::cerr << "❌ Другой экземпляр watchdog уже работает\n";
        return 1;
    }

    RuntimeState state = load_state(cfg.state_file);
    std::cout << "🛡️ Автономный watchdog: " << cfg.health_url
              << ", сохранённый режим=" << route_name(state.route) << '\n';

    keep_running = true;
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    while (keep_running) {
        const bool healthy = check_primary(cfg);
        const std::int64_t now = epoch_now();
        const Decision decision = observe(state, cfg, healthy, now);

        int apply_rc = 0;
        if (decision != Decision::None) {
            const Route desired = decision == Decision::UsePrimary ? Route::Primary : Route::Backup;
            std::cout << "🔄 Применяю маршрут " << route_name(desired)
                      << (state.reconciled ? "" : " (сверка после запуска)") << '\n';
            apply_rc = apply_decision(cfg, decision);
            if (apply_rc == 0) {
                state.route = desired;
                state.last_switch = now;
                state.reconciled = true;
            } else {
                std::cerr << "❌ switch script завершился с кодом " << apply_rc << '\n';
            }
        }

        std::string state_error;
        if (!save_state(cfg.state_file, state, state_error)) {
            std::cerr << "❌ " << state_error << '\n';
            if (once) return 1;
        }
        if (once) {
            ::close(lock_fd);
            return apply_rc == 0 ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(cfg.interval_sec));
    }
    ::close(lock_fd);
    return 0;
}

int print_status(const AgentConfig& cfg) {
    const RuntimeState state = load_state(cfg.state_file);
    std::cout << "mode=" << route_name(state.route) << '\n'
              << "primary_healthy=" << (state.primary_healthy ? "true" : "false") << '\n'
              << "last_check=" << state.last_check << '\n'
              << "last_switch=" << state.last_switch << '\n';
    return state.route == Route::Unknown ? 3 : 0;
}

void usage() {
    std::cout
        << "madweb-agent watchdog [--config=PATH] [--once]\n"
        << "madweb-agent status   [--config=PATH]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string action = "watchdog";
    std::string config_path = "/etc/madwebmirror/watchdog.conf";
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "watchdog" || arg == "status") action = arg;
        else if (arg == "--once") once = true;
        else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(std::string("--config=").size());
        } else {
            std::cerr << "❌ Неизвестный аргумент: " << arg << '\n';
            usage();
            return 2;
        }
    }

    try {
        const AgentConfig cfg = load_config(config_path);
        std::string error;
        if (!validate(cfg, error)) {
            std::cerr << "❌ Ошибка конфигурации агента: " << error << '\n';
            return 2;
        }
        if (action == "status") return print_status(cfg);
        return run_watchdog(cfg, once);
    } catch (const std::exception& e) {
        std::cerr << "❌ " << e.what() << '\n';
        return 1;
    }
}
