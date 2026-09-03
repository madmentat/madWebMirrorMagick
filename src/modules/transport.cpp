#include "mad/transport.hpp"

#include <stdexcept>
#include <utility>

namespace mad {

SshTransportMode parse_transport_mode(const std::string& value) {
    if (value.empty() || value == "direct") return SshTransportMode::Direct;
    if (value == "jump") return SshTransportMode::Jump;
    if (value == "auto") return SshTransportMode::Auto;
    throw std::runtime_error("ssh_transport должен быть direct, jump или auto");
}

const char* transport_mode_name(SshTransportMode mode) {
    switch (mode) {
        case SshTransportMode::Direct: return "direct";
        case SshTransportMode::Jump: return "jump";
        case SshTransportMode::Auto: return "auto";
    }
    return "direct";
}

TransportProfile transport_profile_from_config(const Config& cfg) {
    TransportProfile profile;
    profile.mode = parse_transport_mode(cfg.ssh_transport);
    profile.identity_file = cfg.ssh_identity_file;

    if (!cfg.ssh_jump_primary.empty()) {
        SshJumpRoute route;
        route.id = "proxy-primary";
        route.proxy_jump = cfg.ssh_jump_primary;
        route.identity_file = cfg.ssh_jump_primary_identity_file;
        profile.jump_routes.push_back(std::move(route));
    }
    if (!cfg.ssh_jump_fallback.empty()) {
        SshJumpRoute route;
        route.id = "proxy-fallback";
        route.proxy_jump = cfg.ssh_jump_fallback;
        route.identity_file = cfg.ssh_jump_fallback_identity_file;
        profile.jump_routes.push_back(std::move(route));
    }
    return profile;
}

bool validate_transport_profile(const TransportProfile& profile, std::string& err) {
    err.clear();
    if (profile.mode == SshTransportMode::Jump && profile.jump_routes.empty()) {
        err = "ssh_transport=jump, но не задан ни один jump-host";
        return false;
    }

    for (const auto& route : profile.jump_routes) {
        if (route.proxy_jump.empty()) {
            err = "Пустой proxy_jump в маршруте " + route.id;
            return false;
        }
        for (const auto& tunnel : route.tunnels) {
            if (tunnel.id.empty()) {
                err = "SSH tunnel без id в маршруте " + route.id;
                return false;
            }
            if (tunnel.bind_port < 0 || tunnel.bind_port > 65535 ||
                tunnel.target_port <= 0 || tunnel.target_port > 65535) {
                err = "Некорректный порт SSH tunnel " + tunnel.id;
                return false;
            }
            if (tunnel.target_host.empty()) {
                err = "SSH tunnel " + tunnel.id + ": target_host пуст";
                return false;
            }
        }
    }
    return true;
}

} // namespace mad
