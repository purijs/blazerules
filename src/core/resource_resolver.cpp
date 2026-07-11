#include "blazerules/resource_resolver.h"

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace blazerules {
namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string cache_root() {
    if (const char* env = std::getenv("BLAZERULES_RESOURCE_CACHE")) {
        if (*env) return env;
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.cache/blazerules/resources";
    }
    return ".blazerules/resources";
}

std::string env_value(const char* primary, const char* fallback = nullptr,
                      const char* fallback2 = nullptr) {
    if (const char* env = std::getenv(primary)) {
        if (*env) return env;
    }
    if (fallback) {
        if (const char* env = std::getenv(fallback)) {
            if (*env) return env;
        }
    }
    if (fallback2) {
        if (const char* env = std::getenv(fallback2)) {
            if (*env) return env;
        }
    }
    return {};
}

void set_env_value(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

void clear_env_value(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

std::string basename_for_uri(const std::string& uri) {
    size_t p = uri.find_last_of('/');
    std::string base = p == std::string::npos ? uri : uri.substr(p + 1);
    if (base.empty()) base = "resource";
    for (char& c : base) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) c = '_';
    }
    return base;
}

std::string aws_cli_command_prefix() {
    std::string profile = current_aws_profile();
    std::string region = current_aws_region();
    std::string endpoint_url = current_aws_endpoint_url();
    std::string cmd;
    if (!profile.empty()) {
        // Force profile mode to ignore stale key-based credentials inherited by
        // the process. This matches `aws ... --profile X` from a clean shell.
#ifdef _WIN32
        cmd += "set AWS_ACCESS_KEY_ID=& set AWS_SECRET_ACCESS_KEY=& set AWS_SESSION_TOKEN=& set AWS_SECURITY_TOKEN=& ";
#else
        cmd += "unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN AWS_SECURITY_TOKEN; ";
#endif
    }
    cmd += "aws";
    if (!profile.empty()) {
        cmd += " --profile ";
        cmd += shell_quote(profile);
    }
    if (!region.empty()) {
        cmd += " --region ";
        cmd += shell_quote(region);
    }
    if (!endpoint_url.empty()) {
        cmd += " --endpoint-url ";
        cmd += shell_quote(endpoint_url);
    }
    return cmd;
}

std::string resource_cache_key(const std::string& uri) {
    std::string key = uri;
    key += "\nprofile=";
    key += current_aws_profile();
    key += "\nregion=";
    key += current_aws_region();
    key += "\nendpoint_url=";
    key += current_aws_endpoint_url();
    return key;
}

} // namespace

bool is_s3_uri(const std::string& uri) {
    return uri.rfind("s3://", 0) == 0;
}

std::string resource_parent(const std::string& uri) {
    size_t p = uri.find_last_of('/');
    if (p == std::string::npos) return ".";
    if (is_s3_uri(uri) && p < std::string("s3://").size()) return uri;
    return uri.substr(0, p);
}

std::string join_resource_path(const std::string& base, const std::string& path) {
    if (path.empty()) return base;
    if (is_s3_uri(path)) return path;
    fs::path p(path);
    if (p.is_absolute()) return path;
    if (is_s3_uri(base)) {
        std::string out = base;
        if (!out.empty() && out.back() != '/') out += '/';
        out += path;
        return out;
    }
    return (fs::path(base) / p).string();
}

void set_aws_profile(const std::string& profile, bool clear_env_credentials) {
    set_env_value("BLAZERULES_AWS_PROFILE", profile);
    set_env_value("AWS_PROFILE", profile);
    if (clear_env_credentials) clear_aws_credentials();
}

void clear_aws_profile() {
    clear_env_value("BLAZERULES_AWS_PROFILE");
    clear_env_value("AWS_PROFILE");
}

std::string current_aws_profile() {
    return env_value("BLAZERULES_AWS_PROFILE", "AWS_PROFILE");
}

void set_aws_region(const std::string& region) {
    set_env_value("BLAZERULES_AWS_REGION", region);
    set_env_value("AWS_DEFAULT_REGION", region);
    set_env_value("AWS_REGION", region);
}

void clear_aws_region() {
    clear_env_value("BLAZERULES_AWS_REGION");
    clear_env_value("AWS_DEFAULT_REGION");
    clear_env_value("AWS_REGION");
}

std::string current_aws_region() {
    return env_value("BLAZERULES_AWS_REGION", "AWS_REGION", "AWS_DEFAULT_REGION");
}

void set_aws_endpoint_url(const std::string& endpoint_url) {
    set_env_value("BLAZERULES_AWS_ENDPOINT_URL", endpoint_url);
    set_env_value("AWS_ENDPOINT_URL", endpoint_url);
}

void clear_aws_endpoint_url() {
    clear_env_value("BLAZERULES_AWS_ENDPOINT_URL");
    clear_env_value("AWS_ENDPOINT_URL");
}

std::string current_aws_endpoint_url() {
    return env_value("BLAZERULES_AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_S3");
}

void set_aws_credentials(const std::string& access_key_id,
                         const std::string& secret_access_key,
                         const std::string& session_token,
                         const std::string& region) {
    clear_aws_profile();
    set_env_value("AWS_ACCESS_KEY_ID", access_key_id);
    set_env_value("AWS_SECRET_ACCESS_KEY", secret_access_key);
    set_env_value("AWS_SESSION_TOKEN", session_token);
    set_env_value("AWS_SECURITY_TOKEN", session_token);
    if (!region.empty()) {
        set_aws_region(region);
    }
}

void clear_aws_credentials() {
    clear_env_value("AWS_ACCESS_KEY_ID");
    clear_env_value("AWS_SECRET_ACCESS_KEY");
    clear_env_value("AWS_SESSION_TOKEN");
    clear_env_value("AWS_SECURITY_TOKEN");
}

std::string resolve_resource_to_local(const std::string& uri) {
    if (uri.rfind("file://", 0) == 0) return uri.substr(7);
    if (!is_s3_uri(uri)) return uri;

    const std::string cache_key = resource_cache_key(uri);
    std::hash<std::string> hasher;
    fs::path dir = fs::path(cache_root()) /
                   std::to_string(static_cast<unsigned long long>(hasher(cache_key)));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("failed to create BlazeRules resource cache: " + dir.string());
    }
    fs::path dest = dir / basename_for_uri(uri);
    fs::path meta = dir / "source.uri";

    bool needs_fetch = !fs::exists(dest, ec) || fs::file_size(dest, ec) == 0;
    if (!needs_fetch && fs::exists(meta, ec)) {
        std::ifstream in(meta);
        std::string current;
        std::ostringstream ss;
        ss << in.rdbuf();
        current = ss.str();
        needs_fetch = current != cache_key;
    }

    if (needs_fetch) {
        std::string cmd = aws_cli_command_prefix() + " s3 cp " + shell_quote(uri) + " " +
                          shell_quote(dest.string()) +
                          " >/dev/null 2>/dev/null";
        int rc = std::system(cmd.c_str());
        if (rc != 0 || !fs::exists(dest, ec)) {
            throw std::runtime_error("failed to fetch S3 resource via aws CLI: " + uri);
        }
        std::ofstream out(meta, std::ios::trunc);
        out << cache_key;
    }
    return dest.string();
}

bool s3_upload_file(const std::string& local_path, const std::string& s3_uri) {
    std::string cmd = aws_cli_command_prefix() + " s3 cp " + shell_quote(local_path) + " " +
                      shell_quote(s3_uri) + " >/dev/null 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

bool s3_download_file(const std::string& s3_uri, const std::string& local_path) {
    std::string cmd = aws_cli_command_prefix() + " s3 cp " + shell_quote(s3_uri) + " " +
                      shell_quote(local_path) + " >/dev/null 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

bool s3_sync_up(const std::string& local_dir, const std::string& s3_prefix) {
    std::string cmd = aws_cli_command_prefix() + " s3 sync " + shell_quote(local_dir) + " " +
                      shell_quote(s3_prefix) + " >/dev/null 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

bool s3_sync_down(const std::string& s3_prefix, const std::string& local_dir) {
    std::string cmd = aws_cli_command_prefix() + " s3 sync " + shell_quote(s3_prefix) + " " +
                      shell_quote(local_dir) + " >/dev/null 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

std::string s3_local_cache_dir(const std::string& uri) {
    std::hash<std::string> hasher;
    fs::path dir = fs::path(cache_root()) /
                   ("s3sync-" + std::to_string(static_cast<unsigned long long>(hasher(resource_cache_key(uri)))));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

} // namespace blazerules
