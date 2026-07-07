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
    server.Get("/api/summary", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.summary_json(), "application/json");
    });
    server.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.metrics_json(), "application/json");
    });
    server.Get("/api/decisions", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.decisions_json(static_cast<size_t>(limit_from_request(req, "limit", 500, 5000))),
                        "application/json");
    });
    server.Get("/api/rules", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.rules_json(static_cast<size_t>(limit_from_request(req, "limit", 100, 5000))),
                        "application/json");
    });
    server.Get("/api/errors", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.errors_json(static_cast<size_t>(limit_from_request(req, "limit", 200, 5000))),
                        "application/json");
    });
    server.Get("/api/benchmarks", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.benchmarks_json(), "application/json");
    });
    server.Get("/api/ruleset", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.ruleset_json(), "application/json");
    });
}
