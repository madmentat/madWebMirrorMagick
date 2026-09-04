#include "mad/enroll.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mad {
namespace {

constexpr const char* SERVICE_HOME = "/var/lib/madwebmirror";
constexpr const char* SERVICE_KNOWN_HOSTS = "/var/lib/madwebmirror/.ssh/known_hosts";

struct Endpoint {
    std::string user;
    std::string host;
    int port{22};
};

bool safe_atom(const std::string& s, bool allow_colon) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') continue;
        if (allow_colon && c == ':') continue;
        return false;
    }
    return true;
}

bool parse_endpoint(const std::string& spec, Endpoint& out, std::string& err) {
    out = Endpoint{};
    std::string host_port = spec;
    const auto at = spec.find('@');
    if (at != std::string::npos) {
        if (spec.find('@', at + 1) != std::string::npos) {
            err = "SSH endpoint содержит несколько @";
            return false;
        }
        out.user = spec.substr(0, at);
        host_port = spec.substr(at + 1);
        if (!safe_atom(out.user, false)) {
            err = "Некорректный SSH user в endpoint";
            return false;
        }
    }

    if (!host_port.empty() && host_port.front() == '[') {
        const auto close = host_port.find(']');
        if (close == std::string::npos) {
            err = "Некорректный IPv6 endpoint";
            return false;
        }
        out.host = host_port.substr(1, close - 1);
        if (close + 1 < host_port.size()) {
            if (host_port[close + 1] != ':') {
                err = "После ] ожидается :PORT";
                return false;
            }
            const std::string p = host_port.substr(close + 2);
            try {
                std::size_t used = 0;
                out.port = std::stoi(p, &used);
                if (used != p.size()) throw std::invalid_argument("tail");
            } catch (...) {
                err = "Некорректный SSH port";
                return false;
            }
        }
    } else {
        const auto first_colon = host_port.find(':');
        const auto last_colon = host_port.rfind(':');
        if (first_colon != std::string::npos && first_colon != last_colon) {
            err = "IPv6 endpoint укажите как [addr]:port";
            return false;
        }
        if (last_colon != std::string::npos) {
            out.host = host_port.substr(0, last_colon);
            const std::string p = host_port.substr(last_colon + 1);
            try {
                std::size_t used = 0;
                out.port = std::stoi(p, &used);
                if (used != p.size()) throw std::invalid_argument("tail");
            } catch (...) {
                err = "Некорректный SSH port";
                return false;
            }
        } else {
            out.host = host_port;
        }
    }

    if (!safe_atom(out.host, true)) {
        err = "Некорректный hostname/IP";
        return false;
    }
    if (out.port <= 0 || out.port > 65535) {
        err = "SSH port должен быть 1..65535";
        return false;
    }
    return true;
}

std::string safe_id(std::string value) {
    for (char& c : value) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '-';
    }
    while (!value.empty() && value.front() == '-') value.erase(value.begin());
    while (!value.empty() && value.back() == '-') value.pop_back();
    return value.empty() ? "node" : value;
}

std::string ssh_host(const std::string& host) {
    return host.find(':') == std::string::npos ? host : "[" + host + "]";
}

passwd* service_user() {
    return ::getpwnam("madbackup");
}

void chown_to_service_user(const std::filesystem::path& path) {
    if (::geteuid() != 0) return;
    passwd* pw = service_user();
    if (!pw) return;
    if (::chown(path.c_str(), pw->pw_uid, pw->pw_gid) != 0) {
        std::cerr << "⚠️ Не удалось передать " << path << " пользователю madbackup\n";
    }
}

std::string service_command_prefix() {
    // Enrollment initiated from sudo madUI should still write known_hosts as
    // the long-running service account, not into /root/.ssh.
    if (::geteuid() == 0 && service_user() && has_command("sudo")) {
        return "sudo -u madbackup -H ";
    }
    return {};
}

bool ensure_trust_store(std::string& err) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(SERVICE_HOME) / ".ssh";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = "Не удалось создать SSH trust directory: " + ec.message();
        return false;
    }
    ::chmod(dir.c_str(), 0700);
    chown_to_service_user(dir);
    return true;
}

bool ensure_key(const std::string& path, const std::string& label, std::string& err) {
    namespace fs = std::filesystem;
    if (path.empty()) {
        err = "Пустой путь SSH identity";
        return false;
    }
    if (!has_command("ssh-keygen")) {
        err = "Не найден ssh-keygen (openssh-client)";
        return false;
    }

    const fs::path key(path);
    std::error_code ec;
    fs::create_directories(key.parent_path(), ec);
    if (ec) {
        err = "Не удалось создать каталог SSH keys: " + ec.message();
        return false;
    }
    ::chmod(key.parent_path().c_str(), 0700);
    chown_to_service_user(key.parent_path());

    if (!fs::exists(key)) {
        const std::string cmd = "umask 077; ssh-keygen -q -t ed25519 -N '' -C " +
                                shell_quote("madwebmirror:" + label) + " -f " + shell_quote(path);
        if (run_local(cmd, false) != 0) {
            err = "ssh-keygen завершился ошибкой для " + path;
            return false;
        }
    }

    const fs::path pub(path + ".pub");
    if (!fs::exists(pub)) {
        const std::string cmd = "ssh-keygen -y -f " + shell_quote(path) + " > " + shell_quote(pub.string());
        if (run_local(cmd, false) != 0) {
            err = "Не удалось восстановить публичный ключ " + pub.string();
            return false;
        }
    }

    ::chmod(key.c_str(), 0600);
    ::chmod(pub.c_str(), 0644);
    chown_to_service_user(key);
    chown_to_service_user(pub);
    return true;
}

std::string proxy_command(const Endpoint& jump, const std::string& identity) {
    std::ostringstream cmd;
    cmd << "ssh -o BatchMode=yes -o StrictHostKeyChecking=yes"
        << " -o UserKnownHostsFile=" << shell_quote(SERVICE_KNOWN_HOSTS)
        << " -o ConnectTimeout=10";
    if (!identity.empty()) cmd << " -o IdentitiesOnly=yes -i " << shell_quote(identity);
    if (!jump.user.empty()) cmd << " -l " << shell_quote(jump.user);
    cmd << " -p " << jump.port << " -W %h:%p " << shell_quote(jump.host);
    return cmd.str();
}

int copy_key_direct(const Endpoint& endpoint, const std::string& public_key) {
    std::ostringstream cmd;
    cmd << service_command_prefix()
        << "ssh-copy-id -i " << shell_quote(public_key)
        << " -o StrictHostKeyChecking=ask"
        << " -o UserKnownHostsFile=" << shell_quote(SERVICE_KNOWN_HOSTS)
        << " -p " << endpoint.port << ' ';
    const std::string target = endpoint.user.empty()
        ? ssh_host(endpoint.host)
        : endpoint.user + "@" + ssh_host(endpoint.host);
    cmd << shell_quote(target);
    return run_local(cmd.str(), true);
}

int copy_target_key(const Config& cfg, const std::string& jump_spec,
                    const std::string& jump_identity) {
    Endpoint target;
    std::string err;
    target.user = cfg.remote_user;
    target.host = cfg.remote_host;
    target.port = cfg.ssh_port;
    if (!safe_atom(target.user, false) || !safe_atom(target.host, true) ||
        target.port <= 0 || target.port > 65535) {
        std::cerr << "❌ Некорректный target SSH endpoint\n";
        return 1;
    }

    std::ostringstream cmd;
    cmd << service_command_prefix()
        << "ssh-copy-id -i " << shell_quote(cfg.ssh_identity_file + ".pub")
        << " -o StrictHostKeyChecking=ask"
        << " -o UserKnownHostsFile=" << shell_quote(SERVICE_KNOWN_HOSTS);

    if (!jump_spec.empty()) {
        Endpoint jump;
        if (!parse_endpoint(jump_spec, jump, err)) {
            std::cerr << "❌ " << err << '\n';
            return 1;
        }
        cmd << " -o ProxyCommand=" << shell_quote(proxy_command(jump, jump_identity));
    }

    cmd << " -p " << target.port << ' '
        << shell_quote(target.user + "@" + ssh_host(target.host));
    return run_local(cmd.str(), true);
}

} // namespace

bool ensure_ssh_identities(Config& cfg, std::string& err) {
    if (!ensure_trust_store(err)) return false;

    const std::string base = "/var/lib/madwebmirror/ssh";
    if (cfg.ssh_identity_file.empty()) {
        cfg.ssh_identity_file = base + "/target-" + safe_id(cfg.remote_host);
    }
    if (!ensure_key(cfg.ssh_identity_file, "target-" + cfg.remote_host, err)) return false;

    if (!cfg.ssh_jump_primary.empty()) {
        if (cfg.ssh_jump_primary_identity_file.empty()) {
            cfg.ssh_jump_primary_identity_file = base + "/proxy-a";
        }
        if (!ensure_key(cfg.ssh_jump_primary_identity_file, "proxy-a", err)) return false;
    }
    if (!cfg.ssh_jump_fallback.empty()) {
        if (cfg.ssh_jump_fallback_identity_file.empty()) {
            cfg.ssh_jump_fallback_identity_file = base + "/proxy-b";
        }
        if (!ensure_key(cfg.ssh_jump_fallback_identity_file, "proxy-b", err)) return false;
    }
    return true;
}

int enroll_ssh_interactive(Config& cfg, const std::string& config_path) {
    if (!::isatty(STDIN_FILENO)) {
        std::cerr << "❌ SSH enrollment требует интерактивный терминал для первого password/host-key handshake.\n";
        return 1;
    }
    if (!has_command("ssh-copy-id")) {
        std::cerr << "❌ Не найден ssh-copy-id. Установите openssh-client.\n";
        return 1;
    }

    std::string err;
    if (!ensure_ssh_identities(cfg, err)) {
        std::cerr << "❌ " << err << '\n';
        return 1;
    }
    try {
        save_config(config_path, cfg);
    } catch (const std::exception& e) {
        std::cerr << "❌ Не удалось сохранить пути SSH keys: " << e.what() << '\n';
        return 1;
    }

    std::cout << "\n🔐 SSH enrollment\n"
              << "Пароли, если OpenSSH их запросит, вводятся прямо в этом терминале.\n"
              << "Trust store: " << SERVICE_KNOWN_HOSTS << "\n"
              << "madWebMirrorMagick их не получает и не сохраняет.\n\n";

    if (!cfg.ssh_jump_primary.empty()) {
        Endpoint ep;
        if (!parse_endpoint(cfg.ssh_jump_primary, ep, err)) {
            std::cerr << "❌ Proxy A: " << err << '\n';
            return 1;
        }
        std::cout << "▶ Устанавливаю ключ Proxy A: " << cfg.ssh_jump_primary << '\n';
        if (copy_key_direct(ep, cfg.ssh_jump_primary_identity_file + ".pub") != 0) return 1;
    }

    if (!cfg.ssh_jump_fallback.empty()) {
        Endpoint ep;
        if (!parse_endpoint(cfg.ssh_jump_fallback, ep, err)) {
            std::cerr << "❌ Proxy B: " << err << '\n';
            return 1;
        }
        std::cout << "▶ Устанавливаю ключ Proxy B: " << cfg.ssh_jump_fallback << '\n';
        if (copy_key_direct(ep, cfg.ssh_jump_fallback_identity_file + ".pub") != 0) return 1;
    }

    if (cfg.ssh_transport == "direct" || cfg.ssh_transport == "auto") {
        std::cout << "▶ Устанавливаю ключ конечного узла напрямую: "
                  << cfg.remote_user << '@' << cfg.remote_host << ':' << cfg.ssh_port << '\n';
        if (copy_target_key(cfg, {}, {}) == 0) {
            std::cout << "✅ SSH enrollment завершён.\n";
            return 0;
        }
        if (cfg.ssh_transport == "direct") return 1;
        std::cerr << "⚠️ Direct enrollment не удался, пробую Proxy A/B.\n";
    }

    if (!cfg.ssh_jump_primary.empty()) {
        std::cout << "▶ Устанавливаю target key через Proxy A\n";
        if (copy_target_key(cfg, cfg.ssh_jump_primary, cfg.ssh_jump_primary_identity_file) == 0) {
            std::cout << "✅ SSH enrollment завершён через Proxy A.\n";
            return 0;
        }
    }
    if (!cfg.ssh_jump_fallback.empty()) {
        std::cout << "▶ Устанавливаю target key через Proxy B\n";
        if (copy_target_key(cfg, cfg.ssh_jump_fallback, cfg.ssh_jump_fallback_identity_file) == 0) {
            std::cout << "✅ SSH enrollment завершён через Proxy B.\n";
            return 0;
        }
    }

    std::cerr << "❌ Не удалось установить target key ни по одному SSH-маршруту.\n";
    return 1;
}

} // namespace mad
