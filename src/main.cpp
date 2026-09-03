#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "mad/backup.hpp"
#include "mad/core.hpp"
#include "mad/daemon.hpp"
#include "mad/enroll.hpp"
#include "mad/tunnels.hpp"
#include "mad/ui.hpp"

namespace fs = std::filesystem;

namespace {

enum class Action {
    Backup,
    BackupDaemon,
    Monitor,
    Ui,
    Enroll,
    Tunnels,
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
        if (arg == "ui" || arg == "--ui" || arg == "madui") return Action::Ui;
        if (arg == "enroll" || arg == "ssh-enroll") return Action::Enroll;
        if (arg == "tunnels" || arg == "tunnel-supervisor") return Action::Tunnels;
        if (arg == "--daemon") return Action::BackupDaemon;
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

std::string explicit_tunnels_path(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* prefix = "--tunnels=";
        if (arg.rfind(prefix, 0) == 0) return arg.substr(std::char_traits<char>::length(prefix));
    }
    return mad::TUNNELS_PATH;
}

mad::UiOptions ui_options(int argc, char** argv) {
    mad::UiOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--ui-bind=", 0) == 0) {
            options.bind = arg.substr(std::string("--ui-bind=").size());
        } else if (arg.rfind("--ui-port=", 0) == 0) {
            const std::string value = arg.substr(std::string("--ui-port=").size());
            try {
                std::size_t used = 0;
                options.port = std::stoi(value, &used);
                if (used != value.size()) throw std::invalid_argument("tail");
            } catch (...) {
                throw std::runtime_error("Некорректный --ui-port=" + value);
            }
        }
    }
    return options;
}

std::string user_config_path() {
    const char* home = std::getenv("HOME");
    if (home && *home) return (fs::path(home) / ".config" / "madbackuper.conf").string();
    return mad::CFG_PATH_FALLBACK;
}

void print_help() {
    std::cout << R"(madWebMirrorMagick

Использование:
  madbackuper backup                 Один бэкап + развёртывание (по умолчанию)
  madbackuper --daemon               Плановый backup по schedule_hhmm
  madbackuper monitor                Health monitor / failover
  sudo madbackuper ui                Интерактивная панель madUI
  sudo madbackuper enroll            Создать SSH keys и выполнить первый ssh-copy-id
  madbackuper tunnels                Supervisor SSH-туннелей Proxy A/Proxy B
  madbackuper install                Установить systemd backup timer + remote watchdog
  madbackuper uninstall              Удалить сервисы

madUI:
  sudo madbackuper ui --ui-bind=0.0.0.0 --ui-port=8790

  После открытия URL браузер показывает одноразовый код. Новый browser-session
  получает доступ только после подтверждения [y/N] в том же sudo-терминале.
  Sudo/root пароль приложение не получает и не сохраняет.

SSH transport:
  --ssh-transport=direct|jump|auto
  --ssh-identity-file=/var/lib/madwebmirror/ssh/target-node
  --ssh-jump-primary=madbackup@proxy-a.example.net:22
  --ssh-jump-primary-identity-file=/var/lib/madwebmirror/ssh/proxy-a
  --ssh-jump-fallback=madbackup@proxy-b.example.net:22
  --ssh-jump-fallback-identity-file=/var/lib/madwebmirror/ssh/proxy-b

Tunnel manager:
  madbackuper tunnels --tunnels=/etc/madwebmirror/tunnels.conf

  Одинаковый tunnel id для primary/fallback образует failover-группу: активен
  один SSH forward, а при его падении supervisor переключается на второй Proxy.

Общие параметры:
  --config=/path/to/file
  --remote-host=...
  --remote-user=...
  --ssh-port=22
  --skip-tar --skip-sql --skip-upload
  --schedule=HH:MM

SSH host key должен быть проверен. Enrollment использует OpenSSH host-key prompt;
пароли первого подключения вводятся непосредственно в терминал и не сохраняются.
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
        std::cout << "✅ Создан конфиг: " << cfg_path << '\n';
        if (action != Action::Ui) {
            std::cout << "Запустите sudo madbackuper ui и завершите настройку через madUI.\n";
            return 0;
        }
    }

    mad::Config cfg;
    try {
        mad::load_kv_file(cfg_path, cfg);
        mad::apply_cli_kv(argc, argv, cfg);
    } catch (const std::exception& e) {
        std::cerr << "❌ Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    }

    // madUI должен уметь стартовать даже с ещё не законченной конфигурацией:
    // именно через него пользователь исправляет параметры первого запуска.
    if (action == Action::Ui) {
        try {
            return mad::run_ui(cfg, cfg_path, ui_options(argc, argv));
        } catch (const std::exception& e) {
            std::cerr << "❌ madUI: " << e.what() << '\n';
            return 1;
        }
    }

    // Enrollment тоже должен работать до полной настройки сайта/БД.
    if (action == Action::Enroll) {
        if (cfg.remote_host.empty() || cfg.remote_user.empty() || cfg.ssh_port <= 0 || cfg.ssh_port > 65535) {
            std::cerr << "❌ Для enrollment нужны remote_host, remote_user и корректный ssh_port.\n";
            return 1;
        }
        return mad::enroll_ssh_interactive(cfg, cfg_path);
    }

    // Tunnel supervisor зависит только от SSH Proxy-профилей и tunnels.conf,
    // а не от готовности web/db backup-конфигурации.
    if (action == Action::Tunnels) {
        return mad::run_tunnel_supervisor(cfg, explicit_tunnels_path(argc, argv));
    }

    std::string validation_error;
    if (!mad::validate(cfg, validation_error)) {
        std::cerr << "❌ Ошибка параметров: " << validation_error
                  << "\nЗапустите sudo madbackuper ui для настройки.\n";
        return 1;
    }

    switch (action) {
        case Action::Backup:       return mad::run_backup_once(cfg);
        case Action::BackupDaemon: return mad::run_backup_daemon(cfg);
        case Action::Monitor:      return mad::run_monitor_loop(cfg);
        case Action::Install:      return mad::daemon_install(cfg);
        case Action::Uninstall:    return mad::daemon_uninstall(cfg);
        case Action::Ui:
        case Action::Enroll:
        case Action::Tunnels:
        case Action::Help:         break;
    }
    return 1;
}
