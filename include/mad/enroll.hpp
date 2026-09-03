#pragma once

#include <string>

#include "mad/core.hpp"

namespace mad {

// Generates missing Ed25519 identities for the target and both configured
// proxy routes. Paths are written back into cfg and should then be persisted.
bool ensure_ssh_identities(Config& cfg, std::string& err);

// Interactive first enrollment. Passwords, when needed, are read directly by
// OpenSSH from the administrative terminal and are never returned to or stored
// by madWebMirrorMagick.
int enroll_ssh_interactive(Config& cfg, const std::string& config_path);

} // namespace mad
