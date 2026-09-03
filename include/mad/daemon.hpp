#pragma once
#include <string>
#include "mad/core.hpp"
#include "mad/net.hpp"

namespace mad {

// Установка/удаление демона (локально + оркестратор на proxy, если настроен)
int daemon_install(const Config& cfg, const std::string& self_path);
int daemon_uninstall(const Config& cfg);

// Запускает фоновой цикл демона-монитора: health-check main/зеркал/proxy,
// супервизия оркестратора, плановые бэкапы по расписанию.
int run_daemon_loop(const Config& cfg);

// Однократный отчёт о состоянии (для --status): main, зеркала, proxy.
int status_report(const Config& cfg);

// Универсальный health-check: curl -fsSL -m 5 [ -H "Host: <host_header>" ] <url>
bool check_url(const std::string& url, const std::string& host_header);

// ── Оркестратор (реализация в proxy.cpp, режим --proxy) ──
// Объявлены здесь, т.к. proxy.hpp может появиться позже (другой агент).
// Слабые символы: если proxy.cpp ещё не собран в бинарь, вызовы безопасно
// пропускаются (проверка `if (proxy_install)`).
__attribute__((weak)) int proxy_install(const Config& cfg);
__attribute__((weak)) int proxy_uninstall(const Config& cfg);

} // namespace mad