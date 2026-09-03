#pragma once

#include <string>
#include <vector>

#include "mad/core.hpp"
#include "mad/transport.hpp"

namespace mad {

inline constexpr const char* TUNNELS_PATH = "/etc/madwebmirror/tunnels.conf";

struct ManagedTunnel {
    // Same id on primary and fallback means two alternative routes for one
    // logical tunnel. Only one route in a group is active at a time.
    std::string id;
    std::string route; // primary | fallback
    SshTunnelSpec spec;
};

bool load_tunnels(const std::string& path, std::vector<ManagedTunnel>& tunnels, std::string& err);
bool save_tunnels(const std::string& path, const std::vector<ManagedTunnel>& tunnels, std::string& err);
bool validate_tunnels(const Config& cfg, const std::vector<ManagedTunnel>& tunnels, std::string& err);

// Long-running OpenSSH tunnel supervisor. Each logical tunnel prefers Proxy A
// and automatically retries through Proxy B when its active ssh process exits.
int run_tunnel_supervisor(const Config& cfg, const std::string& path = TUNNELS_PATH);

} // namespace mad
