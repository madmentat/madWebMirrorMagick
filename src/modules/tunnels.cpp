#include "mad/tunnels.hpp"

#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mad {
namespace {

std::atomic<bool> keep_running{true};

void on_signal(int) {
    keep_running = false;
}

struct Endpoint {
    std::string user;
    std::string host;
    int port{22};
};

bool safe_atom(const std::string& s, bool allow_colon = false) {
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
            err = "endpoint содержит несколько @";
            return false;
        }
        out.user = spec.substr(0, at);
        host_port = spec.substr(at + 1);
        if (!safe_atom(out.user)) {
            err = "Некорректный SSH user";
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
            try {
                std::size_t used = 0;
                const std::string p = host_port.substr(close + 2);
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
            try {
                std::size_t used = 0;
                const std::string p = host_port.substr(last_colon + 1);
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

    if (!safe_atom(out.host, true) || out.port <= 0 || out.port > 65535) {
        err = "Некорректный SSH endpoint";
        return false;
    }
    return true;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const auto next = s.find(delim, pos);
        out.push_back(s.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return out;
}

bool parse_int(const std::string& s, int& out) {
    try {
        std::size_t used = 0;
        out = std::stoi(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

bool parse_bool(const std::string& s) {
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string direction_name(SshTunnelDirection direction) {
    return direction == SshTunnelDirection::LocalForward ? "local" : "remote";
}

std::string route_spec(const Config& cfg, const std::string& route) {
    if (route == "primary") return cfg.ssh_jump_primary;
    if (route == "fallback") return cfg.ssh_jump_fallback;
    return {};
}

std::string route_key(const Config& cfg, const std::string& route) {
    if (route == "primary") return cfg.ssh_jump_primary_identity_file;
    if (route == "fallback") return cfg.ssh_jump_fallback_identity_file;
    return {};
}

void chown_to_service_user(const std::filesystem::path& path) {
    if (::geteuid() != 0) return;
    passwd* pw = ::getpwnam("madbackup");
    if (!pw) return;
    ::chown(path.c_str(), pw->pw_uid, pw->pw_gid);
}

std::string forward_arg(const ManagedTunnel& t) {
    std::ostringstream out;
    out << t.spec.bind_host << ':' << t.spec.bind_port << ':'
        << t.spec.target_host << ':' << t.spec.target_port;
    return out.str();
}

pid_t spawn_tunnel(const Config& cfg, const ManagedTunnel& tunnel, std::string& err) {
    Endpoint ep;
    const std::string endpoint_spec = route_spec(cfg, tunnel.route);
    if (!parse_endpoint(endpoint_spec, ep, err)) return -1;

    const std::string identity = route_key(cfg, tunnel.route);
    if (identity.empty()) {
        err = "Для маршрута " + tunnel.route + " не задан SSH identity";
        return -1;
    }

    std::vector<std::string> args = {
        "ssh", "-N", "-T",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=yes",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ConnectTimeout=10",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=2",
        "-o", "IdentitiesOnly=yes",
        "-i", identity,
        "-p", std::to_string(ep.port)
    };
    if (!ep.user.empty()) {
        args.push_back("-l");
        args.push_back(ep.user);
    }
    args.push_back(tunnel.spec.direction == SshTunnelDirection::LocalForward ? "-L" : "-R");
    args.push_back(forward_arg(tunnel));
    args.push_back(ep.host);

    pid_t pid = ::fork();
    if (pid < 0) {
        err = "fork() failed";
        return -1;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execvp("ssh", argv.data());
        _exit(127);
    }
    return pid;
}

struct GroupState {
    std::string id;
    std::vector<ManagedTunnel> routes;
    std::size_t next_route{0};
    pid_t pid{-1};
    std::string active_route;
    std::chrono::steady_clock::time_point retry_at{};
};

void stop_child(GroupState& group) {
    if (group.pid <= 0) return;
    ::kill(group.pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 10; ++i) {
        if (::waitpid(group.pid, &status, WNOHANG) == group.pid) {
            group.pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(group.pid, SIGKILL);
    ::waitpid(group.pid, &status, 0);
    group.pid = -1;
}

} // namespace

bool load_tunnels(const std::string& path, std::vector<ManagedTunnel>& tunnels, std::string& err) {
    tunnels.clear();
    err.clear();
    std::ifstream in(path);
    if (!in) {
        if (!std::filesystem::exists(path)) return true;
        err = "Не удалось открыть " + path;
        return false;
    }

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.rfind("tunnel=", 0) != 0) continue;
        const auto parts = split(line.substr(7), '|');
        if (parts.size() != 8) {
            err = path + ':' + std::to_string(line_no) + ": ожидается 8 полей tunnel";
            return false;
        }
        ManagedTunnel t;
        t.id = parts[0];
        t.route = parts[1];
        if (parts[2] == "local") t.spec.direction = SshTunnelDirection::LocalForward;
        else if (parts[2] == "remote") t.spec.direction = SshTunnelDirection::RemoteForward;
        else {
            err = path + ':' + std::to_string(line_no) + ": direction должен быть local или remote";
            return false;
        }
        t.spec.id = t.id;
        t.spec.bind_host = parts[3];
        if (!parse_int(parts[4], t.spec.bind_port)) {
            err = path + ':' + std::to_string(line_no) + ": bind_port не число";
            return false;
        }
        t.spec.target_host = parts[5];
        if (!parse_int(parts[6], t.spec.target_port)) {
            err = path + ':' + std::to_string(line_no) + ": target_port не число";
            return false;
        }
        t.spec.enabled = parse_bool(parts[7]);
        tunnels.push_back(std::move(t));
    }
    return true;
}

bool save_tunnels(const std::string& path, const std::vector<ManagedTunnel>& tunnels, std::string& err) {
    err.clear();
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
        err = "Не удалось создать каталог tunnels config: " + ec.message();
        return false;
    }

    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
        err = "Не удалось создать " + tmp;
        return false;
    }
    out << "# id|route(primary/fallback)|direction(local/remote)|bind_host|bind_port|target_host|target_port|enabled\n";
    for (const auto& t : tunnels) {
        out << "tunnel=" << t.id << '|' << t.route << '|'
            << direction_name(t.spec.direction) << '|'
            << t.spec.bind_host << '|' << t.spec.bind_port << '|'
            << t.spec.target_host << '|' << t.spec.target_port << '|'
            << (t.spec.enabled ? "true" : "false") << '\n';
    }
    out.flush();
    if (!out) {
        err = "Ошибка записи " + tmp;
        return false;
    }
    out.close();
    ::chmod(tmp.c_str(), 0640);
    chown_to_service_user(tmp);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        err = "Не удалось заменить tunnels config: " + ec.message();
        return false;
    }
    ::chmod(path.c_str(), 0640);
    chown_to_service_user(path);
    return true;
}

bool validate_tunnels(const Config& cfg, const std::vector<ManagedTunnel>& tunnels, std::string& err) {
    err.clear();
    std::set<std::string> local_binds;
    std::map<std::string, std::set<std::string>> seen_routes;

    for (const auto& t : tunnels) {
        if (!safe_atom(t.id) || (t.route != "primary" && t.route != "fallback")) {
            err = "Некорректный tunnel id/route: " + t.id;
            return false;
        }
        if (route_spec(cfg, t.route).empty()) {
            err = "Tunnel " + t.id + " ссылается на незаданный Proxy " + t.route;
            return false;
        }
        if (route_key(cfg, t.route).empty()) {
            err = "Tunnel " + t.id + ": у Proxy " + t.route + " не задан identity key";
            return false;
        }
        if (!safe_atom(t.spec.bind_host, true) || !safe_atom(t.spec.target_host, true) ||
            t.spec.bind_port <= 0 || t.spec.bind_port > 65535 ||
            t.spec.target_port <= 0 || t.spec.target_port > 65535) {
            err = "Некорректный host/port tunnel " + t.id;
            return false;
        }
        if (!seen_routes[t.id].insert(t.route).second) {
            err = "Tunnel " + t.id + " дважды определён для route " + t.route;
            return false;
        }

        // Alternative routes with the same id may intentionally bind the same
        // local port. Different logical tunnel ids may not.
        if (t.spec.enabled && t.spec.direction == SshTunnelDirection::LocalForward) {
            const std::string bind = t.spec.bind_host + ':' + std::to_string(t.spec.bind_port);
            const std::string key = t.id + "@" + bind;
            (void)key;
            for (const auto& other : tunnels) {
                if (&other == &t || !other.spec.enabled || other.id == t.id ||
                    other.spec.direction != SshTunnelDirection::LocalForward) continue;
                if (other.spec.bind_host == t.spec.bind_host && other.spec.bind_port == t.spec.bind_port) {
                    err = "Локальный bind " + bind + " используется tunnel " + t.id + " и " + other.id;
                    return false;
                }
            }
            local_binds.insert(bind);
        }
    }
    return true;
}

int run_tunnel_supervisor(const Config& cfg, const std::string& path) {
    if (!has_command("ssh")) {
        std::cerr << "❌ Tunnel supervisor требует openssh-client.\n";
        return 1;
    }

    std::vector<ManagedTunnel> tunnels;
    std::string err;
    if (!load_tunnels(path, tunnels, err)) {
        std::cerr << "❌ " << err << '\n';
        return 1;
    }
    if (!validate_tunnels(cfg, tunnels, err)) {
        std::cerr << "❌ " << err << '\n';
        return 1;
    }

    std::map<std::string, GroupState> grouped;
    for (const auto& t : tunnels) {
        if (!t.spec.enabled) continue;
        auto& g = grouped[t.id];
        g.id = t.id;
        g.routes.push_back(t);
    }
    for (auto& [id, g] : grouped) {
        (void)id;
        std::stable_sort(g.routes.begin(), g.routes.end(), [](const ManagedTunnel& a, const ManagedTunnel& b) {
            return a.route == "primary" && b.route != "primary";
        });
    }

    if (grouped.empty()) {
        std::cout << "ℹ️ Нет включённых SSH tunnels в " << path << '\n';
        return 0;
    }

    keep_running = true;
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    std::cout << "🛰️ Tunnel supervisor: " << grouped.size() << " failover group(s)\n";

    while (keep_running) {
        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, g] : grouped) {
            if (g.pid > 0) {
                int status = 0;
                const pid_t done = ::waitpid(g.pid, &status, WNOHANG);
                if (done == 0) continue;
                if (done == g.pid) {
                    std::cerr << "⚠️ Tunnel " << id << " route " << g.active_route
                              << " завершился; переключаю маршрут.\n";
                    g.pid = -1;
                    g.next_route = (g.next_route + 1) % g.routes.size();
                    g.retry_at = now + std::chrono::seconds(2);
                }
            }

            if (g.pid <= 0 && now >= g.retry_at) {
                bool started = false;
                for (std::size_t tries = 0; tries < g.routes.size(); ++tries) {
                    const std::size_t idx = (g.next_route + tries) % g.routes.size();
                    const auto& candidate = g.routes[idx];
                    std::string spawn_err;
                    const pid_t pid = spawn_tunnel(cfg, candidate, spawn_err);
                    if (pid <= 0) {
                        std::cerr << "⚠️ Tunnel " << id << " route " << candidate.route
                                  << ": " << spawn_err << '\n';
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(350));
                    int status = 0;
                    const pid_t early = ::waitpid(pid, &status, WNOHANG);
                    if (early == pid) {
                        std::cerr << "⚠️ Tunnel " << id << " route " << candidate.route
                                  << " не поднялся, пробую следующий.\n";
                        continue;
                    }
                    g.pid = pid;
                    g.next_route = idx;
                    g.active_route = candidate.route;
                    std::cout << "✅ Tunnel " << id << " UP via Proxy "
                              << (candidate.route == "primary" ? "A" : "B") << " (pid " << pid << ")\n";
                    started = true;
                    break;
                }
                if (!started) {
                    g.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    std::cerr << "❌ Tunnel " << id << ": оба Proxy-маршрута недоступны; retry через 5s.\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (auto& [id, g] : grouped) {
        (void)id;
        stop_child(g);
    }
    std::cout << "🛑 Tunnel supervisor остановлен.\n";
    return 0;
}

} // namespace mad
