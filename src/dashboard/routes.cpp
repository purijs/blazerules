#include "routes.h"

#include <algorithm>
#include <cstdlib>

#include "assets.h"

namespace {

int limit_from_request(const httplib::Request& req, const char* name, int fallback, int max_value) {
    if (!req.has_param(name)) return fallback;
    int v = std::atoi(req.get_param_value(name).c_str());
    if (v <= 0) return fallback;
    return std::min(v, max_value);
}

int64_t int64_from_request(const httplib::Request& req, const char* name, int64_t fallback = 0) {
    if (!req.has_param(name)) return fallback;
    char* end = nullptr;
    const std::string value = req.get_param_value(name);
    long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str()) return fallback;
    return static_cast<int64_t>(parsed);
}

std::string string_from_request(const httplib::Request& req, const char* name) {
    return req.has_param(name) ? req.get_param_value(name) : std::string{};
}

bool bool_from_request(const httplib::Request& req, const char* name) {
    if (!req.has_param(name)) return false;
    std::string value = req.get_param_value(name);
    return value == "1" || value == "true" || value == "yes";
}

void set_static_content(httplib::Response& res, std::string_view body, const char* content_type) {
    res.set_content(std::string(body), content_type);
}

} // namespace

void register_routes(httplib::Server& server, DashboardServer& dashboard) {
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        set_static_content(res, dashboard_assets::index_html(), "text/html; charset=utf-8");
    });
    server.Get("/assets/styles.css", [](const httplib::Request&, httplib::Response& res) {
        set_static_content(res, dashboard_assets::styles_css(), "text/css; charset=utf-8");
    });
    server.Get("/assets/app.js", [](const httplib::Request&, httplib::Response& res) {
        set_static_content(res, dashboard_assets::app_js(), "application/javascript; charset=utf-8");
    });
    server.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    server.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.health_json(), "application/json");
    });
    server.Get("/api/summary", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.summary_json(string_from_request(req, "instance")), "application/json");
    });
    server.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.metrics_json(), "application/json");
    });
    server.Get("/api/decisions", [&](const httplib::Request& req, httplib::Response& res) {
        DecisionQuery query;
        query.limit = static_cast<size_t>(limit_from_request(req, "limit", 500, 5000));
        query.offset = static_cast<size_t>(std::max<int64_t>(0, int64_from_request(req, "offset", 0)));
        query.scan_file = bool_from_request(req, "scan");
        query.decision = string_from_request(req, "decision");
        query.risk_band = string_from_request(req, "risk_band");
        query.rule = string_from_request(req, "rule");
        query.instance = string_from_request(req, "instance");
        query.from_ms = int64_from_request(req, "from_ms", 0);
        query.to_ms = int64_from_request(req, "to_ms", 0);
        res.set_content(dashboard.decisions_json(query), "application/json");
    });
    server.Get("/api/rules", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.rules_json(static_cast<size_t>(limit_from_request(req, "limit", 100, 5000)),
                                             string_from_request(req, "instance")),
                        "application/json");
    });
    server.Get("/api/models", [&](const httplib::Request& req, httplib::Response& res) {
        int bins = static_cast<int>(limit_from_request(req, "bins", 24, 100));
        res.set_content(dashboard.models_json(bins, string_from_request(req, "instance")), "application/json");
    });
    server.Get("/api/errors", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.errors_json(static_cast<size_t>(limit_from_request(req, "limit", 200, 5000))),
                        "application/json");
    });
    server.Get("/api/benchmarks", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.benchmarks_json(), "application/json");
    });
    server.Get("/api/ruleset", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.ruleset_json(string_from_request(req, "ruleset")), "application/json");
    });
}
