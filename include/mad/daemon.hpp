#pragma once

#include <string>

#include "mad/core.hpp"

namespace mad {

bool check_local_site_ok(const Config& cfg);
int remote_switch_to_local_nginx(const Config& cfg);
int remote_switch_to_remote_nginx(const Config& cfg);
int run_monitor_loop(const Config& cfg);

int daemon_install(const Config& cfg, const std::string& self_path = {});
int daemon_uninstall(const Config& cfg);

} // namespace mad
