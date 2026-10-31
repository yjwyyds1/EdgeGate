#include "edgegate/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "edgegate/log.hpp"
#include "edgegate/http.hpp"

namespace eg {
namespace {

struct RawSection {
    std::string name;
    int line = 0;
    std::map<std::string, std::string> kv;
};

bool parse_bool(std::string_view text, bool& out) {
    if (iequals(text, "true") || iequals(text, "yes") || text == "1" || iequals(text, "on")) {
        out = true;
        return true;
    }
    if (iequals(text, "false") || iequals(text, "no") || text == "0" || iequals(text, "off")) {
        out = false;
        return true;
    }
    return false;
}

bool parse_size(std::string_view text, std::size_t& out) {
    std::string s = trim(text);
    if (s.empty()) return false;
    std::uint64_t multiplier = 1;
    const char last = s.back();
    if (last == 'k' || last == 'K') { multiplier = 1024ull; s.pop_back(); }
    else if (last == 'm' || last == 'M') { multiplier = 1024ull * 1024; s.pop_back(); }
    else if (last == 'g' || last == 'G') { multiplier = 1024ull * 1024 * 1024; s.pop_back(); }
    std::uint64_t v = 0;
    if (!parse_u64(trim(s), v)) return false;
    if (v > (std::numeric_limits<std::uint64_t>::max() / multiplier)) return false;
    out = static_cast<std::size_t>(v * multiplier);
    return true;
}

std::string lower_key(std::string_view s) { return to_lower(trim(s)); }

}  // namespace

bool Config::load(const std::string& path, Config& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open config file: " + path;
        return false;
    }

    std::map<std::string, std::string> global;
    std::map<std::string, int> global_line;
    std::vector<RawSection> sections;
    RawSection* current = nullptr;
    std::set<std::string> section_names;

    std::string raw_line;
    int line_no = 0;
    while (std::getline(in, raw_line)) {
        ++line_no;
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        const std::size_t hash = raw_line.find('#');
        if (hash != std::string::npos) raw_line.erase(hash);
        const std::string line = trim(raw_line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                error = "line " + std::to_string(line_no) + ": section header must end with ']'";
                return false;
            }
            std::string header = trim(line.substr(1, line.size() - 2));
            if (header.rfind("backend ", 0) != 0) {
                error = "line " + std::to_string(line_no) + ": unknown section [" + header + "]";
                return false;
            }
            const std::string name = trim(header.substr(8));
            if (name.empty() || !valid_token(name)) {
                error = "line " + std::to_string(line_no) + ": invalid backend name";
                return false;
            }
            if (!section_names.insert(name).second) {
                error = "line " + std::to_string(line_no) + ": duplicate backend '" + name + "'";
                return false;
            }
            sections.push_back(RawSection{name, line_no, {}});
            current = &sections.back();
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(line_no) + ": expected key = value";
            return false;
        }
        const std::string key = lower_key(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key.empty()) {
            error = "line " + std::to_string(line_no) + ": empty key";
            return false;
        }
        auto& target = current ? current->kv : global;
        if (!target.emplace(key, value).second) {
            error = "line " + std::to_string(line_no) + ": duplicate key '" + key + "'";
            return false;
        }
        if (!current) global_line[key] = line_no;
    }

    std::set<std::string> used;
    auto take = [&](const std::string& key, std::string& value) -> bool {
        auto it = global.find(key);
        if (it == global.end()) return false;
        value = it->second;
        used.insert(key);
        return true;
    };
    auto fail = [&](const std::string& key, const std::string& why) {
        auto it = global_line.find(key);
        const int ln = it == global_line.end() ? 0 : it->second;
        error = "line " + std::to_string(ln) + ": " + key + ": " + why;
        return false;
    };
    auto take_i64 = [&](const std::string& key, std::int64_t& field, std::int64_t min,
                        std::int64_t max) -> bool {
        std::string v;
        if (!take(key, v)) return true;
        std::int64_t parsed = 0;
        if (!parse_i64(v, parsed) || parsed < min || parsed > max) {
            return fail(key, "expected integer in [" + std::to_string(min) + ", " +
                                 std::to_string(max) + "]");
        }
        field = parsed;
        return true;
    };
    auto take_size = [&](const std::string& key, std::size_t& field, std::size_t min,
                         std::size_t max) -> bool {
        std::string v;
        if (!take(key, v)) return true;
        std::size_t parsed = 0;
        if (!parse_size(v, parsed) || parsed < min || parsed > max) {
            return fail(key, "expected size (bytes, K/M/G suffix allowed)");
        }
        field = parsed;
        return true;
    };
    auto take_bool = [&](const std::string& key, bool& field) -> bool {
        std::string v;
        if (!take(key, v)) return true;
        bool parsed = false;
        if (!parse_bool(v, parsed)) return fail(key, "expected true/false");
        field = parsed;
        return true;
    };
    auto take_string = [&](const std::string& key, std::string& field) -> bool {
        std::string v;
        if (!take(key, v)) return true;
        field = v;
        return true;
    };

    std::string listen_text;
    if (take("listen", listen_text)) {
        std::string why;
        if (!parse_endpoint(listen_text, out.listen, why)) return fail("listen", why);
    }
    std::string admin_text;
    if (take("admin_listen", admin_text)) {
        if (iequals(admin_text, "off") || iequals(admin_text, "none")) {
            out.admin_enabled = false;
        } else {
            std::string why;
            if (!parse_endpoint(admin_text, out.admin, why)) return fail("admin_listen", why);
        }
    }
    if (!take_string("balancer", out.balancer)) return false;
    if (out.balancer != "round_robin" && out.balancer != "least_conn") {
        return fail("balancer", "expected round_robin or least_conn");
    }
    if (!take_bool("preserve_host", out.preserve_host)) return false;
    if (!take_bool("retry_idempotent_only", out.retry_idempotent_only)) return false;
    auto take_int = [&](const std::string& key, int& field, int min, int max) -> bool {
        std::string v;
        if (!take(key, v)) return true;
        std::int64_t parsed = 0;
        if (!parse_i64(v, parsed) || parsed < min || parsed > max) {
            return fail(key, "expected integer in [" + std::to_string(min) + ", " +
                                 std::to_string(max) + "]");
        }
        field = static_cast<int>(parsed);
        return true;
    };
    out.max_attempts = 2;
    if (!take_int("max_attempts", out.max_attempts, 1, 10)) return false;
    if (!take_size("retry_body_buffer_bytes", out.retry_body_buffer_bytes, 0, 1u << 30)) return false;

    if (!take_i64("client_header_timeout_ms", out.client_header_timeout_ms, 1, 3600000)) return false;
    if (!take_i64("client_idle_timeout_ms", out.client_idle_timeout_ms, 1, 86400000)) return false;
    if (!take_i64("client_write_stall_ms", out.client_write_stall_ms, 1, 86400000)) return false;
    if (!take_i64("upstream_connect_timeout_ms", out.upstream_connect_timeout_ms, 1, 3600000)) return false;
    if (!take_i64("upstream_response_timeout_ms", out.upstream_response_timeout_ms, 1, 86400000)) return false;
    if (!take_i64("request_timeout_ms", out.request_timeout_ms, 1, 86400000)) return false;
    if (!take_i64("shutdown_grace_ms", out.shutdown_grace_ms, 0, 600000)) return false;

    if (!take_size("max_request_header_bytes", out.max_request_header_bytes, 1024, 1u << 24)) return false;
    if (!take_size("max_request_body_bytes", out.max_request_body_bytes, 0, 1ull << 40)) return false;
    if (!take_size("max_response_header_bytes", out.max_response_header_bytes, 1024, 1u << 24)) return false;

    if (!take_size("client_buffer_high_watermark", out.client_buffer_high, 4096, 1u << 30)) return false;
    if (!take_size("client_buffer_low_watermark", out.client_buffer_low, 1024, 1u << 30)) return false;
    if (!take_size("upstream_buffer_high_watermark", out.upstream_buffer_high, 4096, 1u << 30)) return false;
    if (!take_size("upstream_buffer_low_watermark", out.upstream_buffer_low, 1024, 1u << 30)) return false;
    if (!take_size("max_total_buffered_bytes", out.max_total_buffered, 1u << 20, 1ull << 40)) return false;

    if (!take_size("max_connections_per_backend", out.max_connections_per_backend, 1, 1000000)) return false;
    if (!take_size("max_idle_connections_per_backend", out.max_idle_connections_per_backend, 0, 1000000)) return false;
    if (!take_i64("backend_idle_timeout_ms", out.backend_idle_timeout_ms, 1, 86400000)) return false;

    if (!take_i64("health_interval_ms", out.health_interval_ms, 0, 3600000)) return false;
    if (!take_i64("health_timeout_ms", out.health_timeout_ms, 1, 600000)) return false;
    if (!take_string("health_path", out.health_path)) return false;
    if (!take_int("healthy_threshold", out.healthy_threshold, 1, 100)) return false;
    if (!take_int("unhealthy_threshold", out.unhealthy_threshold, 1, 100)) return false;
    if (!take_i64("passive_failure_cooldown_ms", out.passive_failure_cooldown_ms, 0, 3600000)) return false;

    if (!take_string("access_log", out.access_log)) return false;
    if (!take_string("log_level", out.log_level)) return false;
    if (!out.health_path.empty() && out.health_path.front() != '/') {
        return fail("health_path", "must start with /");
    }
    if (out.client_buffer_low >= out.client_buffer_high) {
        return fail("client_buffer_low_watermark", "must be smaller than high watermark");
    }
    if (out.upstream_buffer_low >= out.upstream_buffer_high) {
        return fail("upstream_buffer_low_watermark", "must be smaller than high watermark");
    }
    if (out.client_buffer_high + out.upstream_buffer_high > out.max_total_buffered) {
        return fail("max_total_buffered_bytes", "must exceed one connection's high watermarks");
    }

    for (const auto& [key, line] : global_line) {
        if (!used.count(key)) {
            error = "line " + std::to_string(line) + ": unknown key '" + key + "'";
            return false;
        }
    }

    std::size_t index = 0;
    for (const RawSection& section : sections) {
        BackendConfig backend;
        backend.name = section.name;
        std::set<std::string> local_used;
        auto local = [&](const std::string& key, std::string& value) -> bool {
            auto it = section.kv.find(key);
            if (it == section.kv.end()) return false;
            value = it->second;
            local_used.insert(key);
            return true;
        };
        std::string url;
        if (!local("url", url)) {
            error = "line " + std::to_string(section.line) + ": backend '" + section.name +
                    "' requires url";
            return false;
        }
        std::string why;
        if (!parse_endpoint(url, backend.endpoint, why)) {
            error = "line " + std::to_string(section.line) + ": backend '" + section.name +
                    "': " + why;
            return false;
        }
        backend.url = url;
        std::string value;
        if (local("max_connections", value)) {
            if (!parse_size(value, backend.max_connections) || backend.max_connections == 0) {
                error = "line " + std::to_string(section.line) + ": backend '" + section.name +
                        "': invalid max_connections";
                return false;
            }
        }
        if (local("max_idle_connections", value)) {
            if (!parse_size(value, backend.max_idle)) {
                error = "line " + std::to_string(section.line) + ": backend '" + section.name +
                        "': invalid max_idle_connections";
                return false;
            }
        }
        for (const auto& [key, value_text] : section.kv) {
            (void)value_text;
            if (!local_used.count(key)) {
                error = "line " + std::to_string(section.line) + ": backend '" + section.name +
                        "': unknown key '" + key + "'";
                return false;
            }
        }
        out.backends.push_back(std::move(backend));
        ++index;
    }
    if (out.backends.empty()) {
        error = "config has no [backend ...] sections";
        return false;
    }

    return true;
}

std::string Config::summary() const {
    std::ostringstream os;
    os << "listen=" << listen.authority() << " admin="
       << (admin_enabled ? admin.authority() : "off") << " balancer=" << balancer
       << " backends=" << backends.size() << " max_attempts=" << max_attempts
       << " preserve_host=" << (preserve_host ? "true" : "false")
       << " client_buffer=" << client_buffer_low << "/" << client_buffer_high
       << " upstream_buffer=" << upstream_buffer_low << "/" << upstream_buffer_high
       << " health_interval_ms=" << health_interval_ms;
    for (const BackendConfig& b : backends) {
        os << " backend[" << b.name << "]=" << b.endpoint.authority();
    }
    return os.str();
}

}  // namespace eg
