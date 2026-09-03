#include "mad/ui.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mad {
namespace {

constexpr std::size_t MAX_HEADER = 64 * 1024;
constexpr std::size_t MAX_BODY = 128 * 1024;
constexpr auto REQUEST_TTL = std::chrono::minutes(5);

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string remote_ip;
};

struct PendingBrowser {
    enum class State { Pending, Approved, Denied, Expired };
    std::string id;
    std::string code;
    std::string ip;
    std::string agent;
    std::string token;
    State state{State::Pending};
    std::chrono::steady_clock::time_point created{std::chrono::steady_clock::now()};
};

std::mutex auth_mutex;
std::condition_variable auth_cv;
std::map<std::string, PendingBrowser> pending;
std::deque<std::string> approval_queue;
std::unordered_set<std::string> sessions;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string json_escape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string random_string(std::size_t n, const char* alphabet) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 rng(rd());
    const std::size_t len = std::strlen(alphabet);
    std::uniform_int_distribution<std::size_t> pick(0, len - 1);
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(alphabet[pick(rng)]);
    return out;
}

std::string random_hex(std::size_t n) {
    return random_string(n, "0123456789abcdef");
}

std::string verification_code() {
    const std::string raw = random_string(8, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
    return raw.substr(0, 4) + "-" + raw.substr(4);
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+' ) {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = s.substr(i + 1, 2);
            char* end = nullptr;
            const long v = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(v));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_form(const std::string& body) {
    std::unordered_map<std::string, std::string> result;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const auto amp = body.find('&', pos);
        const auto part = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = part.find('=');
        const auto key = url_decode(part.substr(0, eq));
        const auto value = eq == std::string::npos ? std::string{} : url_decode(part.substr(eq + 1));
        if (!key.empty()) result[key] = value;
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return result;
}

std::unordered_map<std::string, std::string> parse_query(const std::string& query) {
    return parse_form(query);
}

std::string cookie_value(const HttpRequest& req, const std::string& name) {
    const auto it = req.headers.find("cookie");
    if (it == req.headers.end()) return {};
    std::size_t pos = 0;
    while (pos < it->second.size()) {
        const auto semi = it->second.find(';', pos);
        auto part = trim(it->second.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        const auto eq = part.find('=');
        if (eq != std::string::npos && trim(part.substr(0, eq)) == name) return trim(part.substr(eq + 1));
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return {};
}

bool authorized(const HttpRequest& req) {
    const std::string token = cookie_value(req, "madui_session");
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lock(auth_mutex);
    return sessions.find(token) != sessions.end();
}

bool same_origin(const HttpRequest& req) {
    const auto origin = req.headers.find("origin");
    if (origin == req.headers.end() || origin->second.empty()) return true;
    const auto host = req.headers.find("host");
    if (host == req.headers.end()) return false;
    return origin->second == "http://" + host->second || origin->second == "https://" + host->second;
}

std::string peer_ip(const sockaddr_storage& addr) {
    char host[NI_MAXHOST]{};
    const socklen_t len = addr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&addr), len, host, sizeof(host), nullptr, 0,
                      NI_NUMERICHOST) == 0) return host;
    return "?";
}

bool read_request(int fd, const std::string& remote_ip, HttpRequest& req) {
    std::string data;
    data.reserve(4096);
    char buf[4096];
    std::size_t header_end = std::string::npos;

    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, static_cast<std::size_t>(n));
        if (data.size() > MAX_HEADER) return false;
    }

    const std::string head = data.substr(0, header_end);
    std::istringstream lines(head);
    std::string first;
    if (!std::getline(lines, first)) return false;
    if (!first.empty() && first.back() == '\r') first.pop_back();
    std::istringstream first_line(first);
    std::string version;
    if (!(first_line >> req.method >> req.target >> version)) return false;
    if (version.rfind("HTTP/1.", 0) != 0) return false;

    const auto q = req.target.find('?');
    req.path = req.target.substr(0, q);
    req.query = q == std::string::npos ? std::string{} : req.target.substr(q + 1);
    req.remote_ip = remote_ip;

    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    std::size_t content_length = 0;
    const auto cl = req.headers.find("content-length");
    if (cl != req.headers.end()) {
        try { content_length = static_cast<std::size_t>(std::stoul(cl->second)); }
        catch (...) { return false; }
    }
    if (content_length > MAX_BODY) return false;

    req.body = data.substr(header_end + 4);
    while (req.body.size() < content_length) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        req.body.append(buf, static_cast<std::size_t>(n));
        if (req.body.size() > MAX_BODY) return false;
    }
    if (req.body.size() > content_length) req.body.resize(content_length);
    return true;
}

void send_all(int fd, const std::string& data) {
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::send(fd, p, left, MSG_NOSIGNAL);
        if (n <= 0) return;
        p += n;
        left -= static_cast<std::size_t>(n);
    }
}

void respond(int fd, int status, const std::string& content_type, const std::string& body,
             const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
                         status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
                         status == 409 ? "Conflict" : "Internal Server Error";
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "X-Frame-Options: DENY\r\n"
        << "Referrer-Policy: no-referrer\r\n"
        << "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'\r\n";
    for (const auto& [k, v] : extra) out << k << ": " << v << "\r\n";
    out << "Connection: close\r\n\r\n" << body;
    send_all(fd, out.str());
}

std::vector<std::string> local_ipv4() {
    std::vector<std::string> ips;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return ips;
    for (ifaddrs* p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN]{};
        const auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) && std::string(buf) != "127.0.0.1") {
            ips.emplace_back(buf);
        }
    }
    ::freeifaddrs(list);
    std::sort(ips.begin(), ips.end());
    ips.erase(std::unique(ips.begin(), ips.end()), ips.end());
    return ips;
}

std::string hostname() {
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return "server";
}

std::string ui_html() {
    return R"MADUI(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>madUI · madWebMirrorMagick</title>
<style>
:root{--bg:#07101c;--panel:rgba(14,27,45,.72);--panel2:rgba(20,39,62,.62);--line:rgba(146,199,255,.16);--text:#edf7ff;--muted:#8ea7bb;--a:#75f2d0;--b:#7f9cff;--c:#ff85c8;--ok:#71f6b5;--warn:#ffd36d;--danger:#ff718f;--shadow:0 30px 90px rgba(0,0,0,.42)}
*{box-sizing:border-box}html,body{margin:0;min-height:100%;font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--bg);color:var(--text)}
body{overflow-x:hidden;background:
radial-gradient(900px 600px at 12% -5%,rgba(78,112,255,.24),transparent 65%),
radial-gradient(800px 620px at 95% 8%,rgba(255,77,174,.16),transparent 62%),
radial-gradient(900px 700px at 55% 105%,rgba(54,229,194,.12),transparent 65%),#07101c}
body:before{content:"";position:fixed;inset:0;pointer-events:none;opacity:.3;background-image:linear-gradient(rgba(255,255,255,.025) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.025) 1px,transparent 1px);background-size:36px 36px;mask-image:linear-gradient(to bottom,black,transparent 90%)}
.shell{display:grid;grid-template-columns:248px minmax(0,1fr);min-height:100vh}.side{position:sticky;top:0;height:100vh;padding:26px 18px;border-right:1px solid var(--line);background:rgba(5,12,22,.6);backdrop-filter:blur(24px)}
.brand{display:flex;align-items:center;gap:12px;padding:3px 8px 28px}.orb{width:38px;height:38px;border-radius:14px;background:conic-gradient(from 215deg,var(--a),var(--b),var(--c),var(--a));box-shadow:0 0 42px rgba(117,242,208,.22);position:relative}.orb:after{content:"m";position:absolute;inset:3px;display:grid;place-items:center;border-radius:11px;background:#08111e;font-weight:900;font-size:22px}.brand b{font-size:20px;letter-spacing:-.04em}.brand span{display:block;color:var(--muted);font-size:10px;text-transform:uppercase;letter-spacing:.16em;margin-top:2px}
.nav{display:grid;gap:7px}.nav button{all:unset;display:flex;align-items:center;gap:11px;padding:11px 12px;border-radius:12px;color:#91a9bb;cursor:pointer;font-size:13px}.nav button:hover,.nav button.active{color:#f4fbff;background:linear-gradient(90deg,rgba(117,242,208,.12),rgba(127,156,255,.08));box-shadow:inset 0 0 0 1px rgba(137,210,255,.09)}.doticon{width:8px;height:8px;border-radius:50%;border:1px solid currentColor}.nav button.active .doticon{background:var(--a);border-color:var(--a);box-shadow:0 0 18px var(--a)}
.sidefoot{position:absolute;bottom:24px;left:18px;right:18px;padding:14px;border:1px solid var(--line);border-radius:14px;background:rgba(255,255,255,.025)}.sidefoot small{color:var(--muted)}.secure{margin-top:7px;color:var(--ok);font-size:12px}
.main{padding:34px clamp(24px,4vw,58px) 70px;max-width:1600px;width:100%}.top{display:flex;justify-content:space-between;align-items:flex-start;gap:18px;margin-bottom:28px}.eyebrow{font-size:11px;letter-spacing:.18em;color:var(--a);text-transform:uppercase;font-weight:800}.top h1{font-size:clamp(34px,4.2vw,62px);letter-spacing:-.055em;line-height:.96;margin:8px 0 10px;background:linear-gradient(110deg,#fff 10%,#c9e2ff 52%,#9ff2dc);-webkit-background-clip:text;color:transparent}.sub{color:var(--muted);max-width:690px;line-height:1.6;font-size:14px}.pill{padding:9px 13px;border:1px solid rgba(113,246,181,.22);background:rgba(113,246,181,.07);border-radius:999px;color:var(--ok);font-size:12px;white-space:nowrap}.pill:before{content:"";display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--ok);margin-right:8px;box-shadow:0 0 14px var(--ok)}
.grid{display:grid;grid-template-columns:1.45fr .85fr;gap:18px}.card{border:1px solid var(--line);background:linear-gradient(145deg,rgba(20,37,60,.75),rgba(8,18,31,.72));box-shadow:var(--shadow);border-radius:22px;backdrop-filter:blur(20px);overflow:hidden}.cardhead{display:flex;align-items:center;justify-content:space-between;padding:19px 21px;border-bottom:1px solid rgba(255,255,255,.055)}.cardhead b{font-size:13px}.muted{color:var(--muted);font-size:12px}
.topology{position:relative;min-height:340px;padding:30px}.topology:before{content:"";position:absolute;inset:0;background:radial-gradient(circle at 50% 50%,rgba(98,138,255,.11),transparent 52%)}.beam{position:absolute;top:50%;left:27%;right:27%;height:1px;background:linear-gradient(90deg,var(--a),var(--b),var(--c));box-shadow:0 0 18px rgba(117,242,208,.5)}.beam:after{content:"";position:absolute;width:70px;height:3px;top:-1px;left:-70px;background:linear-gradient(90deg,transparent,#fff,transparent);filter:blur(.3px);animation:flow 2.5s linear infinite}@keyframes flow{to{left:calc(100% + 70px)}}
.node{position:absolute;top:50%;transform:translateY(-50%);width:190px;padding:18px;border-radius:20px;border:1px solid rgba(255,255,255,.11);background:rgba(8,20,35,.86);box-shadow:0 20px 50px rgba(0,0,0,.36)}.node.primary{left:28px}.node.mirror{right:28px}.node .role{font-size:10px;letter-spacing:.16em;text-transform:uppercase;color:var(--muted)}.node h3{margin:7px 0 4px;font-size:18px;letter-spacing:-.03em}.node .ip{font:12px ui-monospace,SFMono-Regular,Menlo,monospace;color:#a9c7da}.statusline{display:flex;gap:7px;align-items:center;margin-top:13px;color:var(--ok);font-size:11px}.statusline i{width:7px;height:7px;border-radius:50%;background:var(--ok);box-shadow:0 0 13px var(--ok)}
.metrics{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;padding:0 21px 21px}.metric{padding:15px;border:1px solid rgba(255,255,255,.06);background:rgba(255,255,255,.025);border-radius:15px}.metric small{color:var(--muted);display:block;font-size:10px;text-transform:uppercase;letter-spacing:.1em}.metric b{display:block;margin-top:7px;font-size:17px}
.modules{padding:13px 18px 19px}.module{display:flex;align-items:center;justify-content:space-between;padding:12px 2px;border-bottom:1px solid rgba(255,255,255,.05)}.module:last-child{border:0}.module strong{font-size:12px}.module span{font-size:11px;color:var(--muted)}.switch{width:34px;height:19px;border-radius:20px;background:rgba(117,242,208,.18);border:1px solid rgba(117,242,208,.25);position:relative}.switch:after{content:"";position:absolute;right:3px;top:3px;width:11px;height:11px;border-radius:50%;background:var(--a);box-shadow:0 0 12px var(--a)}.switch.off{background:rgba(255,255,255,.05);border-color:rgba(255,255,255,.08)}.switch.off:after{right:18px;background:#63798b;box-shadow:none}
.setup{grid-column:1/-1}.form{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:13px;padding:21px}.field{display:grid;gap:7px}.field.span2{grid-column:span 2}.field label{font-size:10px;text-transform:uppercase;letter-spacing:.12em;color:#829bb0}.field input,.field select{width:100%;border:1px solid rgba(147,199,255,.13);border-radius:12px;padding:11px 12px;background:rgba(2,9,17,.58);color:#eaf7ff;outline:none}.field input:focus,.field select:focus{border-color:rgba(117,242,208,.55);box-shadow:0 0 0 3px rgba(117,242,208,.07)}.actions{display:flex;justify-content:flex-end;gap:10px;padding:0 21px 21px}.btn{border:1px solid rgba(255,255,255,.1);border-radius:12px;padding:11px 16px;background:rgba(255,255,255,.04);color:#c8dbe9;cursor:pointer;font-weight:700}.btn.primary{border:0;color:#06111b;background:linear-gradient(120deg,var(--a),#9cc7ff);box-shadow:0 10px 30px rgba(117,242,208,.16)}
.toast{position:fixed;right:25px;bottom:25px;padding:13px 16px;border-radius:13px;background:#102338;border:1px solid var(--line);box-shadow:var(--shadow);opacity:0;transform:translateY(12px);pointer-events:none;transition:.25s}.toast.show{opacity:1;transform:none}.verify{position:fixed;inset:0;z-index:50;display:grid;place-items:center;background:rgba(3,8,15,.78);backdrop-filter:blur(18px)}.verify.hidden{display:none}.verifybox{width:min(520px,calc(100vw - 34px));padding:30px;border:1px solid rgba(151,210,255,.17);border-radius:26px;background:linear-gradient(145deg,rgba(21,40,65,.95),rgba(7,16,29,.97));box-shadow:0 40px 110px #000}.verifylogo{width:62px;height:62px;margin-bottom:24px}.verify h2{font-size:28px;letter-spacing:-.045em;margin:0 0 8px}.verify p{color:var(--muted);line-height:1.6;font-size:13px}.code{font:800 28px ui-monospace,SFMono-Regular,Menlo,monospace;letter-spacing:.12em;margin:22px 0;padding:15px;text-align:center;border-radius:15px;background:rgba(117,242,208,.07);border:1px solid rgba(117,242,208,.18);color:#caffef}.wait{display:flex;gap:9px;align-items:center;font-size:12px;color:#bad0df}.spinner{width:13px;height:13px;border:2px solid rgba(255,255,255,.13);border-top-color:var(--a);border-radius:50%;animation:spin .8s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}
@media(max-width:980px){.shell{grid-template-columns:1fr}.side{display:none}.main{padding:24px 16px 60px}.grid{grid-template-columns:1fr}.topology{min-height:430px}.node{left:50%!important;right:auto!important;transform:translate(-50%,-50%)}.node.primary{top:30%}.node.mirror{top:72%}.beam{left:50%;right:auto;top:42%;width:1px;height:120px;background:linear-gradient(var(--a),var(--b),var(--c))}.beam:after{display:none}.form{grid-template-columns:1fr 1fr}.field.span2{grid-column:span 2}}
@media(max-width:620px){.form{grid-template-columns:1fr}.field.span2{grid-column:auto}.metrics{grid-template-columns:1fr}.top{display:block}.pill{display:inline-block;margin-top:10px}}
</style>
</head>
<body>
<div class="verify" id="verify"><div class="verifybox"><div class="orb verifylogo"></div><div class="eyebrow">Terminal trust handshake</div><h2>Подтвердите браузер</h2><p>madUI не хранит sudo-пароль. Доступ выдаётся только после подтверждения этой вкладки в том же административном терминале, где запущена панель.</p><div class="code" id="code">••••-••••</div><div class="wait"><span class="spinner"></span><span id="vtext">Создаём одноразовый запрос…</span></div></div></div>
<div class="shell"><aside class="side"><div class="brand"><div class="orb"></div><div><b>madUI</b><span>Mirror control plane</span></div></div><nav class="nav"><button class="active"><i class="doticon"></i>Обзор</button><button><i class="doticon"></i>Архитектура</button><button><i class="doticon"></i>Узлы</button><button><i class="doticon"></i>Бэкапы</button><button><i class="doticon"></i>Failover</button><button><i class="doticon"></i>Безопасность</button><button><i class="doticon"></i>Настройки</button></nav><div class="sidefoot"><small>Administrative session</small><div class="secure">● terminal verified</div></div></aside>
<main class="main"><header class="top"><div><div class="eyebrow">madWebMirrorMagick</div><h1>Веб-сервер<br>под контролем.</h1><div class="sub">Зеркалирование, резервное копирование, health-check и переключение узлов — в одной панели. Системные пароли не превращаются в постоянные секреты приложения.</div></div><div class="pill">CONTROL PLANE ONLINE</div></header>
<div class="grid"><section class="card"><div class="cardhead"><b>Архитектура зеркала</b><span class="muted" id="hostName">loading…</span></div><div class="topology"><div class="beam"></div><div class="node primary"><div class="role">Primary node</div><h3 id="primaryName">local</h3><div class="ip" id="primaryIp">—</div><div class="statusline"><i></i>контроллер доступен</div></div><div class="node mirror"><div class="role">Mirror node</div><h3 id="mirrorName">mirror</h3><div class="ip" id="mirrorIp">—</div><div class="statusline"><i></i>конфигурация загружена</div></div></div><div class="metrics"><div class="metric"><small>Backup schedule</small><b id="metricSchedule">04:00</b></div><div class="metric"><small>Health interval</small><b id="metricHealth">60 s</b></div><div class="metric"><small>Web stack</small><b id="metricStack">nginx</b></div></div></section>
<section class="card"><div class="cardhead"><b>Модули</b><span class="muted">pipeline</span></div><div class="modules"><div class="module"><div><strong>Файлы сайта</strong><br><span>tar + secure transfer</span></div><div class="switch"></div></div><div class="module"><div><strong>MariaDB / MySQL</strong><br><span>dump + restore</span></div><div class="switch"></div></div><div class="module"><div><strong>Health monitor</strong><br><span>3 failures / recovery</span></div><div class="switch"></div></div><div class="module"><div><strong>Automatic failover</strong><br><span>nginx switch helper</span></div><div class="switch"></div></div><div class="module"><div><strong>TLS sync</strong><br><span>следующий этап</span></div><div class="switch off"></div></div></div></section>
<section class="card setup"><div class="cardhead"><div><b>Конфигурация системы</b><div class="muted" style="margin-top:4px">Это станет мастером первого запуска; пока здесь собраны ключевые параметры текущей архитектуры.</div></div><span class="muted" id="configPath"></span></div><form class="form" id="configForm"><div class="field"><label>Тип веб-сервера</label><select name="target_server" id="target_server"><option>nginx</option><option>apache2</option></select></div><div class="field"><label>Имя сайта</label><input name="server_name" id="server_name"></div><div class="field span2"><label>Локальный webroot</label><input name="local_site_dir" id="local_site_dir"></div><div class="field"><label>Mirror host</label><input name="remote_host" id="remote_host"></div><div class="field"><label>SSH user</label><input name="remote_user" id="remote_user"></div><div class="field"><label>SSH port</label><input name="ssh_port" id="ssh_port" inputmode="numeric"></div><div class="field"><label>Proxy target</label><input name="proxy_target" id="proxy_target"></div><div class="field span2"><label>Remote webroot</label><input name="remote_site_dir" id="remote_site_dir"></div><div class="field span2"><label>Remote backup storage</label><input name="remote_backup_base" id="remote_backup_base"></div><div class="field"><label>Database</label><input name="db_name" id="db_name"></div><div class="field"><label>DB user</label><input name="db_user" id="db_user"></div><div class="field"><label>Backup time</label><input name="schedule_hhmm" id="schedule_hhmm" placeholder="04:00"></div><div class="field"><label>Health interval, sec</label><input name="health_interval_sec" id="health_interval_sec" inputmode="numeric"></div><div class="field span2"><label>Health URL</label><input name="health_url" id="health_url"></div><div class="field span2"><label>Host header (optional)</label><input name="health_host_header" id="health_host_header"></div></form><div class="actions"><button class="btn" id="reloadBtn">Отменить изменения</button><button class="btn primary" id="saveBtn">Сохранить конфигурацию</button></div></section></div></main></div><div class="toast" id="toast"></div>
<script>
const $=id=>document.getElementById(id);let reqId='';
function toast(t,ok=true){const e=$('toast');e.textContent=t;e.style.borderColor=ok?'rgba(113,246,181,.28)':'rgba(255,113,143,.35)';e.classList.add('show');setTimeout(()=>e.classList.remove('show'),2600)}
async function beginAuth(){try{const r=await fetch('/api/session/request',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'browser='+encodeURIComponent(navigator.userAgent)});const j=await r.json();reqId=j.request_id;$('code').textContent=j.code;$('vtext').textContent='Подтвердите этот код в sudo-терминале';pollAuth()}catch(e){$('vtext').textContent='Не удалось создать запрос: '+e}}
async function pollAuth(){try{const r=await fetch('/api/session/status?id='+encodeURIComponent(reqId));const j=await r.json();if(j.state==='approved'){ $('verify').classList.add('hidden');await loadConfig();return }if(j.state==='denied'||j.state==='expired'){ $('vtext').textContent=j.state==='denied'?'Доступ отклонён в терминале':'Код истёк. Обновите страницу.';return }}catch(e){}setTimeout(pollAuth,700)}
function put(id,v){const e=$(id);if(e)e.value=v??''}
async function loadConfig(){const r=await fetch('/api/config');if(!r.ok){location.reload();return}const j=await r.json();Object.entries(j.config).forEach(([k,v])=>put(k,v));$('configPath').textContent=j.config_path;$('hostName').textContent=j.hostname;$('primaryName').textContent=j.hostname;$('primaryIp').textContent=j.local_ip||'local';$('mirrorName').textContent=j.config.remote_host||'not configured';$('mirrorIp').textContent=(j.config.remote_user||'user')+'@'+(j.config.remote_host||'—')+':'+j.config.ssh_port;$('metricSchedule').textContent=j.config.schedule_hhmm;$('metricHealth').textContent=j.config.health_interval_sec+' s';$('metricStack').textContent=j.config.target_server}
$('saveBtn').onclick=async e=>{e.preventDefault();const body=new URLSearchParams(new FormData($('configForm')));const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});let j={};try{j=await r.json()}catch{}if(r.ok){toast('Конфигурация сохранена');loadConfig()}else toast(j.error||'Ошибка сохранения',false)};
$('reloadBtn').onclick=e=>{e.preventDefault();loadConfig();toast('Изменения отменены')};beginAuth();
</script>
</body></html>)MADUI";
}

std::string config_json(const Config& c, const std::string& config_path, const std::string& host) {
    const auto ips = local_ipv4();
    const std::string ip = ips.empty() ? "127.0.0.1" : ips.front();
    std::ostringstream out;
    out << "{\"hostname\":\"" << json_escape(host) << "\","
        << "\"local_ip\":\"" << json_escape(ip) << "\","
        << "\"config_path\":\"" << json_escape(config_path) << "\",\"config\":{"
        << "\"target_server\":\"" << json_escape(c.target_server) << "\","
        << "\"server_name\":\"" << json_escape(c.server_name) << "\","
        << "\"local_site_dir\":\"" << json_escape(c.local_site_dir) << "\","
        << "\"remote_host\":\"" << json_escape(c.remote_host) << "\","
        << "\"remote_user\":\"" << json_escape(c.remote_user) << "\","
        << "\"ssh_port\":" << c.ssh_port << ','
        << "\"proxy_target\":\"" << json_escape(c.proxy_target) << "\","
        << "\"remote_site_dir\":\"" << json_escape(c.remote_site_dir) << "\","
        << "\"remote_backup_base\":\"" << json_escape(c.remote_backup_base) << "\","
        << "\"db_name\":\"" << json_escape(c.db_name) << "\","
        << "\"db_user\":\"" << json_escape(c.db_user) << "\","
        << "\"schedule_hhmm\":\"" << json_escape(c.schedule_hhmm) << "\","
        << "\"health_interval_sec\":" << c.health_interval_sec << ','
        << "\"health_url\":\"" << json_escape(c.health_url) << "\","
        << "\"health_host_header\":\"" << json_escape(c.health_host_header) << "\"}}";
    return out.str();
}

void set_if_present(const std::unordered_map<std::string, std::string>& f, const char* key, std::string& dst) {
    const auto it = f.find(key);
    if (it != f.end()) dst = it->second;
}

bool parse_int_field(const std::unordered_map<std::string, std::string>& f, const char* key, int& dst, std::string& err) {
    const auto it = f.find(key);
    if (it == f.end()) return true;
    try {
        std::size_t used = 0;
        const int value = std::stoi(it->second, &used);
        if (used != it->second.size()) throw std::invalid_argument("tail");
        dst = value;
        return true;
    } catch (...) {
        err = std::string("Некорректное число: ") + key;
        return false;
    }
}

void expire_old_requests() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, p] : pending) {
        (void)id;
        if (p.state == PendingBrowser::State::Pending && now - p.created > REQUEST_TTL) p.state = PendingBrowser::State::Expired;
    }
}

void approval_loop() {
    for (;;) {
        PendingBrowser snapshot;
        {
            std::unique_lock<std::mutex> lock(auth_mutex);
            auth_cv.wait(lock, [] { return !approval_queue.empty(); });
            const std::string id = approval_queue.front();
            approval_queue.pop_front();
            auto it = pending.find(id);
            if (it == pending.end() || it->second.state != PendingBrowser::State::Pending) continue;
            snapshot = it->second;
        }

        std::cout << "\n\033[1;36m╭────────────────── madUI browser request ──────────────────╮\033[0m\n"
                  << "  IP:      " << snapshot.ip << "\n"
                  << "  Browser: " << snapshot.agent.substr(0, 110) << "\n"
                  << "  Code:    \033[1;32m" << snapshot.code << "\033[0m\n"
                  << "\033[1;36m╰────────────────────────────────────────────────────────────╯\033[0m\n"
                  << "Разрешить этому браузеру административный сеанс? [y/N] " << std::flush;

        std::string answer;
        if (!std::getline(std::cin, answer)) answer.clear();
        const bool yes = !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y' || answer[0] == 'д' || answer[0] == 'Д');

        std::lock_guard<std::mutex> lock(auth_mutex);
        auto it = pending.find(snapshot.id);
        if (it == pending.end() || it->second.state != PendingBrowser::State::Pending) continue;
        if (yes) {
            it->second.state = PendingBrowser::State::Approved;
            it->second.token = random_hex(64);
            sessions.insert(it->second.token);
            std::cout << "\033[1;32m✓ Браузер подтверждён. sudo/root пароль не сохранён.\033[0m\n";
        } else {
            it->second.state = PendingBrowser::State::Denied;
            std::cout << "\033[1;31m✗ Запрос отклонён.\033[0m\n";
        }
    }
}

void handle_client(int fd, const std::string& remote_ip, Config& cfg, std::mutex& cfg_mutex,
                   const std::string& config_path, const std::string& host) {
    HttpRequest req;
    if (!read_request(fd, remote_ip, req)) {
        respond(fd, 400, "application/json; charset=utf-8", "{\"error\":\"bad request\"}");
        return;
    }

    if (req.method == "GET" && req.path == "/") {
        respond(fd, 200, "text/html; charset=utf-8", ui_html());
        return;
    }

    if (req.method == "POST" && req.path == "/api/session/request") {
        if (!same_origin(req)) {
            respond(fd, 403, "application/json; charset=utf-8", "{\"error\":\"origin rejected\"}");
            return;
        }
        const auto form = parse_form(req.body);
        PendingBrowser p;
        p.id = random_hex(20);
        p.code = verification_code();
        p.ip = req.remote_ip;
        const auto browser = form.find("browser");
        const auto ua = req.headers.find("user-agent");
        p.agent = browser != form.end() ? browser->second : (ua != req.headers.end() ? ua->second : "unknown");
        {
            std::lock_guard<std::mutex> lock(auth_mutex);
            expire_old_requests();
            pending[p.id] = p;
            approval_queue.push_back(p.id);
        }
        auth_cv.notify_one();
        respond(fd, 202, "application/json; charset=utf-8",
                "{\"request_id\":\"" + p.id + "\",\"code\":\"" + p.code + "\"}");
        return;
    }

    if (req.method == "GET" && req.path == "/api/session/status") {
        const auto q = parse_query(req.query);
        const auto qid = q.find("id");
        if (qid == q.end()) {
            respond(fd, 400, "application/json; charset=utf-8", "{\"error\":\"missing id\"}");
            return;
        }
        std::lock_guard<std::mutex> lock(auth_mutex);
        expire_old_requests();
        const auto it = pending.find(qid->second);
        if (it == pending.end()) {
            respond(fd, 404, "application/json; charset=utf-8", "{\"state\":\"expired\"}");
            return;
        }
        if (it->second.state == PendingBrowser::State::Approved) {
            respond(fd, 200, "application/json; charset=utf-8", "{\"state\":\"approved\"}",
                    {{"Set-Cookie", "madui_session=" + it->second.token + "; Path=/; HttpOnly; SameSite=Strict"}});
        } else if (it->second.state == PendingBrowser::State::Denied) {
            respond(fd, 200, "application/json; charset=utf-8", "{\"state\":\"denied\"}");
        } else if (it->second.state == PendingBrowser::State::Expired) {
            respond(fd, 200, "application/json; charset=utf-8", "{\"state\":\"expired\"}");
        } else {
            respond(fd, 200, "application/json; charset=utf-8", "{\"state\":\"pending\"}");
        }
        return;
    }

    if (!authorized(req)) {
        respond(fd, 401, "application/json; charset=utf-8", "{\"error\":\"terminal verification required\"}");
        return;
    }

    if (req.method == "GET" && req.path == "/api/config") {
        std::lock_guard<std::mutex> lock(cfg_mutex);
        respond(fd, 200, "application/json; charset=utf-8", config_json(cfg, config_path, host));
        return;
    }

    if (req.method == "POST" && req.path == "/api/config") {
        if (!same_origin(req)) {
            respond(fd, 403, "application/json; charset=utf-8", "{\"error\":\"origin rejected\"}");
            return;
        }
        const auto form = parse_form(req.body);
        std::lock_guard<std::mutex> lock(cfg_mutex);
        Config next = cfg;
        set_if_present(form, "target_server", next.target_server);
        set_if_present(form, "server_name", next.server_name);
        set_if_present(form, "local_site_dir", next.local_site_dir);
        set_if_present(form, "remote_host", next.remote_host);
        set_if_present(form, "remote_user", next.remote_user);
        set_if_present(form, "proxy_target", next.proxy_target);
        set_if_present(form, "remote_site_dir", next.remote_site_dir);
        set_if_present(form, "remote_backup_base", next.remote_backup_base);
        set_if_present(form, "db_name", next.db_name);
        set_if_present(form, "db_user", next.db_user);
        set_if_present(form, "schedule_hhmm", next.schedule_hhmm);
        set_if_present(form, "health_url", next.health_url);
        set_if_present(form, "health_host_header", next.health_host_header);
        std::string err;
        if (!parse_int_field(form, "ssh_port", next.ssh_port, err) ||
            !parse_int_field(form, "health_interval_sec", next.health_interval_sec, err)) {
            respond(fd, 400, "application/json; charset=utf-8", "{\"error\":\"" + json_escape(err) + "\"}");
            return;
        }
        if (!validate(next, err)) {
            respond(fd, 400, "application/json; charset=utf-8", "{\"error\":\"" + json_escape(err) + "\"}");
            return;
        }
        try {
            save_config(config_path, next);
            cfg = next;
            respond(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}");
        } catch (const std::exception& e) {
            respond(fd, 500, "application/json; charset=utf-8", "{\"error\":\"" + json_escape(e.what()) + "\"}");
        }
        return;
    }

    respond(fd, 404, "application/json; charset=utf-8", "{\"error\":\"not found\"}");
}

int bind_listener(const UiOptions& options) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(options.port));
    if (::inet_pton(AF_INET, options.bind.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("madUI пока принимает IPv4 bind-адрес, например 0.0.0.0 или 127.0.0.1");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("bind " + options.bind + ":" + std::to_string(options.port) + ": " + e);
    }
    if (::listen(fd, 32) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("listen: " + e);
    }
    return fd;
}

void print_banner(const UiOptions& options) {
    const auto ips = local_ipv4();
    const char* sudo_user = std::getenv("SUDO_USER");
    std::cout << "\n\033[1;35m"
              << "   __  __           ______  __  __  ____\n"
              << "  / /_/ /__  ___   / __/ / / / / / /  _/\n"
              << " / __/ / _ \\/ _ \\ / _// /_/ / /_/ // /  \n"
              << " \\__/_/\\___/_//_//___/\\____/\\____/___/  madUI\n"
              << "\033[0m\n"
              << "Административный терминал: " << (sudo_user && *sudo_user ? sudo_user : "root") << " (euid=0)\n"
              << "madUI слушает " << options.bind << ':' << options.port << "\n\n"
              << "Откройте в браузере:\n";
    if (options.bind == "127.0.0.1") {
        std::cout << "  \033[1;36mhttp://127.0.0.1:" << options.port << "/\033[0m\n";
    } else {
        if (ips.empty()) std::cout << "  \033[1;36mhttp://" << hostname() << ':' << options.port << "/\033[0m\n";
        for (const auto& ip : ips) std::cout << "  \033[1;36mhttp://" << ip << ':' << options.port << "/\033[0m\n";
    }
    std::cout << "\nПосле открытия страницы здесь появится одноразовый код.\n"
              << "Подтвердите его клавишей y. Без подтверждения API недоступен.\n"
              << "Ctrl+C завершает административный сеанс и инвалидирует все browser-session.\n\n";
}

} // namespace

int run_ui(Config cfg, const std::string& config_path, const UiOptions& options) {
    if (::geteuid() != 0) {
        std::cerr << "❌ madUI должен запускаться из административного терминала.\n"
                  << "   Используйте: sudo madwebmirror ui\n"
                  << "   sudo-пароль используется только самим sudo и приложению не передаётся.\n";
        return 1;
    }
    if (!::isatty(STDIN_FILENO)) {
        std::cerr << "❌ madUI требует интерактивный TTY для подтверждения новых браузеров.\n";
        return 1;
    }
    if (options.port <= 0 || options.port > 65535) {
        std::cerr << "❌ ui-port должен быть в диапазоне 1..65535\n";
        return 1;
    }

    int listen_fd = -1;
    try { listen_fd = bind_listener(options); }
    catch (const std::exception& e) {
        std::cerr << "❌ Не удалось запустить madUI: " << e.what() << '\n';
        return 1;
    }

    print_banner(options);
    std::thread(approval_loop).detach();
    std::mutex cfg_mutex;
    const std::string host = hostname();

    for (;;) {
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        const int client = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (client < 0) {
            if (errno == EINTR) continue;
            std::cerr << "❌ accept: " << std::strerror(errno) << '\n';
            continue;
        }
        const std::string ip = peer_ip(addr);
        std::thread([client, ip, &cfg, &cfg_mutex, config_path, host] {
            handle_client(client, ip, cfg, cfg_mutex, config_path, host);
            ::close(client);
        }).detach();
    }
}

} // namespace mad
