#pragma once

#include <string>

#include "mad/core.hpp"

namespace mad {

struct UiOptions {
    std::string bind{"0.0.0.0"};
    int port{8790};
};

// Интерактивная административная панель. Намеренно запускается on-demand
// из sudo-сеанса: новый браузер получает доступ только после подтверждения
// в том же терминале. Секрет sudo при этом не сохраняется.
int run_ui(Config cfg, const std::string& config_path, const UiOptions& options);

} // namespace mad
