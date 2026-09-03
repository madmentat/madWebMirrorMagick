#pragma once

#include <string>
#include <vector>

#include "mad/core.hpp"

namespace mad {

enum class SshTransportMode {
    Direct,
    Jump,
    Auto
};

enum class SshTunnelDirection {
    LocalForward,
    RemoteForward
};

// Persistent tunnel/forward associated with one proxy-capable node.
// Execution is intentionally separate from the model: agents may later run
// these forwards via libssh/OpenSSH/systemd without changing the topology schema.
struct SshTunnelSpec {
    std::string id;
    SshTunnelDirection direction{SshTunnelDirection::LocalForward};
    std::string bind_host{"127.0.0.1"};
    int bind_port{0};
    std::string target_host;
    int target_port{22};
    bool enabled{true};
};

// One alternative jump path. In a dual-proxy installation proxy-a and proxy-b
// are alternative routes, not a serial A -> B chain.
struct SshJumpRoute {
    std::string id;
    std::string proxy_jump;          // OpenSSH/libssh syntax: [user@]host[:port]
    std::string identity_file;       // key used on the jump host
    std::vector<SshTunnelSpec> tunnels;
};

struct TransportProfile {
    SshTransportMode mode{SshTransportMode::Direct};
    std::string identity_file;       // key used on the final target
    std::vector<SshJumpRoute> jump_routes;
};

SshTransportMode parse_transport_mode(const std::string& value);
const char* transport_mode_name(SshTransportMode mode);
TransportProfile transport_profile_from_config(const Config& cfg);
bool validate_transport_profile(const TransportProfile& profile, std::string& err);

} // namespace mad
