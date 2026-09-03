#pragma once

#include "mad/core.hpp"

namespace mad {

int run_backup_once(const Config& cfg);
int run_backup_daemon(const Config& cfg);

} // namespace mad
