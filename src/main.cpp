#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "mad/backup.hpp"
#include "mad/core.hpp"
#include "mad/daemon.hpp"

namespace fs = std::filesystem;

namespace {

enum class Action {
    Backup,
    BackupDaemon,
    Monitor,
    Install,
    Uninstall,
    Help
};

Action parse_action(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "help") return Action::Help;
        if (arg == "--daemon-install" || arg == "install") return Action::Install;
        if (arg == "--daemon-uninstall" || arg == "uninstall") return Action::Uninstall;
        if (arg == "--monitor" || arg == "monitor") return Action::Monitor;
        if (arg == "--daemon") return Action::BackupDaemon; // совместимость со старой семантикой
        if (arg == "backup") return Action::Backup;
    }
    return Action::Backup;
}

std::string explicit_config_path(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* prefix = "--config=";
        if (arg.rfind(prefix, 0) == 0) return arg.substr(std::char_traits<char>::length(prefix));
    }
    return {};
}

std::string user_config_path() {
    const char* home = std::getenv("HOME");
    if (home && *home) return (fs::path(home) / ".config" / "madbackuper.conf").string();
    return mad::CFG_PATH_FALLBACK;
}

void print_help() {
    std::cout << R"(madWebMirrorMagick / madbackuper

Использование:
  madbackuper backup                 Один бэкап + развёртывание (по умолчанию)
  madbackuper --daemon               Старый daemon-режим: запуск backup по schedule_hhmm
  madbackuper monitor                Локальный health monitor/failover
  madbackuper install                Установить systemd backup timer + remote watchdog
  madbackuper uninstall              Удалить сервисы

Общие параметры:
  --config=/path/to/file
  --remote-host=...
  --remote-user=...
  --ssh-port=22
  --skip-tar --skip-sql --skip-upload
  --schedule=HH:MM

SSH host key обязан уже находиться в known_hosts. Сначала проверьте отпечаток сервера.
)";
}

} // namespace

int main(int argc, char** argv) {
    const Action action = parse_action(argc, argv);
    if (action == Action::Help) {
        print_help();
        return 0;
    }

    std::string cfg_path = explicit_config_path(argc, argv);
    if (cfg_path.empty()) {
        if (fs::exists(mad::CFG_PATH_PRIMARY)) cfg_path = mad::CFG_PATH_PRIMARY;
        else cfg_path = user_config_path();
    }

    if (!fs::exists(cfg_path)) {
        try {
            mad::write_default_config(cfg_path);
        } catch (const std::exception& e) {
            std::cerr << "❌ " << e.what() << '\n';
            return 1;
        }
        std::cout << "✅ Создан конфиг: " << cfg_path
                  << "\nОтредактируйте его и запустите программу снова.\n";
        return 0;
    }

    mad::Config cfg;
    try {
        mad::load_kv_file(cfg_path, cfg);
        mad::apply_cli_kv(argc, argv, cfg);
    } catch (const std::exception& e) {
        std::cerr << "❌ Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    }

    std::string validation_error;
    if (!mad::validate(cfg, validation_error)) {
        std::cerr << "❌ Ошибка параметров: " << validation_error << '\n';
        return 1;
    }

    switch (action) {
        case Action::Backup:       return mad::run_backup_once(cfg);
        case Action::BackupDaemon: return mad::run_backup_daemon(cfg);
        case Action::Monitor:      return mad::run_monitor_loop(cfg);
        case Action::Install:      return mad::daemon_install(cfg);
        case Action::Uninstall:    return mad::daemon_uninstall(cfg);
        case Action::Help:         break;
    }
    return 1;
}
