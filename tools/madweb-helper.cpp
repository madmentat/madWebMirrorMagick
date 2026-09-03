#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char* kServiceUser = "madbackup";
constexpr const char* kSiteRoot = "/srv/madwebmirror/sites";

bool safe_token(const std::string& s, std::size_t max_len = 64) {
    if (s.empty() || s.size() > max_len) return false;
    if (!std::isalnum(static_cast<unsigned char>(s.front()))) return false;
    for (const unsigned char c : s) {
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

bool safe_host(const std::string& s) {
    if (s.empty() || s.size() > 253) return false;
    for (const unsigned char c : s) {
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':')) return false;
    }
    return true;
}

bool parse_port(const std::string& s, int& port) {
    try {
        std::size_t used = 0;
        port = std::stoi(s, &used);
        return used == s.size() && port >= 1 && port <= 65535;
    } catch (...) {
        return false;
    }
}

int require_root() {
    if (::geteuid() == 0) return 0;
    std::cerr << "madweb-helper must run as root (normally via sudo -n).\n";
    return 1;
}

passwd* service_account() {
    passwd* pw = ::getpwnam(kServiceUser);
    if (!pw) std::cerr << "Service account '" << kServiceUser << "' does not exist. Run bootstrap first.\n";
    return pw;
}

bool chown_path(const fs::path& p, uid_t uid, gid_t gid) {
    if (::chown(p.c_str(), uid, gid) != 0) {
        std::cerr << "chown(" << p << ") failed: errno=" << errno << '\n';
        return false;
    }
    return true;
}

int prepare_site(const std::string& site_id) {
    if (!safe_token(site_id)) {
        std::cerr << "Invalid site id. Allowed: letters, digits, '.', '-', '_'.\n";
        return 2;
    }
    passwd* pw = service_account();
    if (!pw) return 1;

    const fs::path base = fs::path(kSiteRoot) / site_id;
    const fs::path releases = base / "releases";
    const fs::path shared = base / "shared";
    const fs::path runtime = fs::path("/var/lib/madwebmirror/sites") / site_id;

    std::error_code ec;
    fs::create_directories(releases, ec);
    if (ec) { std::cerr << "Cannot create " << releases << ": " << ec.message() << '\n'; return 1; }
    fs::create_directories(shared, ec);
    if (ec) { std::cerr << "Cannot create " << shared << ": " << ec.message() << '\n'; return 1; }
    fs::create_directories(runtime, ec);
    if (ec) { std::cerr << "Cannot create " << runtime << ": " << ec.message() << '\n'; return 1; }

    ::chmod(base.c_str(), 0750);
    ::chmod(releases.c_str(), 0750);
    ::chmod(shared.c_str(), 0750);
    ::chmod(runtime.c_str(), 0750);

    if (!chown_path(base, 0, pw->pw_gid)) return 1;
    if (!chown_path(releases, pw->pw_uid, pw->pw_gid)) return 1;
    if (!chown_path(shared, pw->pw_uid, pw->pw_gid)) return 1;
    if (!chown_path(runtime, pw->pw_uid, pw->pw_gid)) return 1;

    std::cout << "Prepared site storage: " << base << '\n';
    return 0;
}

int atomic_write(const fs::path& dst, const std::string& content, mode_t mode) {
    const fs::path tmp = dst.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { std::cerr << "Cannot write " << tmp << '\n'; return 1; }
        out << content;
        if (!out.good()) { std::cerr << "Write failed: " << tmp << '\n'; return 1; }
    }
    if (::chmod(tmp.c_str(), mode) != 0) { fs::remove(tmp); return 1; }
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) { fs::remove(tmp); std::cerr << "rename failed: " << ec.message() << '\n'; return 1; }
    return 0;
}

int route_nginx(const std::string& site_id, const std::string& server_name,
                const std::string& backend_host, const std::string& port_s) {
    int port = 0;
    if (!safe_token(site_id) || !safe_host(server_name) || !safe_host(backend_host) || !parse_port(port_s, port)) {
        std::cerr << "Invalid route arguments.\n";
        return 2;
    }

    fs::create_directories("/etc/nginx/conf.d");
    const fs::path dst = fs::path("/etc/nginx/conf.d") / ("madwebmirror-" + site_id + ".conf");
    const std::string upstream = "mwm_" + site_id;
    const std::string content =
        "# Managed by madWebMirrorMagick. Do not edit manually.\n"
        "upstream " + upstream + " {\n"
        "    server " + backend_host + ":" + std::to_string(port) + ";\n"
        "}\n"
        "server {\n"
        "    listen 80;\n"
        "    server_name " + server_name + ";\n"
        "    location / {\n"
        "        proxy_pass http://" + upstream + ";\n"
        "        proxy_set_header Host $host;\n"
        "        proxy_set_header X-Real-IP $remote_addr;\n"
        "        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
        "        proxy_set_header X-Forwarded-Proto $scheme;\n"
        "    }\n"
        "}\n";

    if (atomic_write(dst, content, 0644) != 0) return 1;
    if (std::system("nginx -t") != 0) {
        std::cerr << "nginx -t failed; route file left for inspection, nginx was NOT reloaded.\n";
        return 1;
    }
    return std::system("systemctl reload nginx") == 0 ? 0 : 1;
}

int route_apache(const std::string& site_id, const std::string& server_name,
                 const std::string& backend_host, const std::string& port_s) {
    int port = 0;
    if (!safe_token(site_id) || !safe_host(server_name) || !safe_host(backend_host) || !parse_port(port_s, port)) {
        std::cerr << "Invalid route arguments.\n";
        return 2;
    }

    fs::create_directories("/etc/apache2/sites-available");
    const fs::path dst = fs::path("/etc/apache2/sites-available") / ("madwebmirror-" + site_id + ".conf");
    const std::string content =
        "# Managed by madWebMirrorMagick. Do not edit manually.\n"
        "<VirtualHost *:80>\n"
        "    ServerName " + server_name + "\n"
        "    ProxyPreserveHost On\n"
        "    ProxyPass / http://" + backend_host + ":" + std::to_string(port) + "/\n"
        "    ProxyPassReverse / http://" + backend_host + ":" + std::to_string(port) + "/\n"
        "</VirtualHost>\n";

    if (atomic_write(dst, content, 0644) != 0) return 1;
    const std::string enable = "a2ensite madwebmirror-" + site_id + " >/dev/null";
    if (std::system(enable.c_str()) != 0) return 1;
    if (std::system("apache2ctl configtest") != 0) {
        std::cerr << "apache2ctl configtest failed; Apache was NOT reloaded.\n";
        return 1;
    }
    return std::system("systemctl reload apache2") == 0 ? 0 : 1;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  madweb-helper prepare-site SITE_ID\n"
        << "  madweb-helper route-nginx SITE_ID SERVER_NAME BACKEND_HOST PORT\n"
        << "  madweb-helper route-apache SITE_ID SERVER_NAME BACKEND_HOST PORT\n"
        << "  madweb-helper nginx-reload\n"
        << "  madweb-helper apache-reload\n";
}

} // namespace

int main(int argc, char** argv) {
    if (require_root() != 0) return 1;
    if (argc < 2) { usage(); return 2; }

    const std::string cmd = argv[1];
    if (cmd == "prepare-site" && argc == 3) return prepare_site(argv[2]);
    if (cmd == "route-nginx" && argc == 6) return route_nginx(argv[2], argv[3], argv[4], argv[5]);
    if (cmd == "route-apache" && argc == 6) return route_apache(argv[2], argv[3], argv[4], argv[5]);
    if (cmd == "nginx-reload" && argc == 2) return std::system("nginx -t && systemctl reload nginx") == 0 ? 0 : 1;
    if (cmd == "apache-reload" && argc == 2) return std::system("apache2ctl configtest && systemctl reload apache2") == 0 ? 0 : 1;

    usage();
    return 2;
}
