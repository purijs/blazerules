#include <iostream>

#include <httplib.h>

#include "dashboard_server.h"
#include "options.h"
#include "routes.h"

int main(int argc, char** argv) {
    Options options = parse_args(argc, argv);
    if (options.host == "0.0.0.0") {
        std::cerr << "warning: dashboard has no authentication and is bound to 0.0.0.0\n";
    }

    DashboardServer dashboard(options);
    dashboard.start();

    httplib::Server server;
    register_routes(server, dashboard);

    std::cout << "BlazeRules dashboard listening on http://" << options.host << ":" << options.port << "\n";
    std::cout << "Sources: decision_log=" << (options.decision_log.empty() ? "<none>" : options.decision_log)
              << " dead_letter_log=" << (options.dead_letter_log.empty() ? "<none>" : options.dead_letter_log)
              << " metrics_url=" << (options.metrics_url.empty() ? "<none>" : options.metrics_url)
              << " results_jsonl=" << (options.results_jsonl.empty() ? "<none>" : options.results_jsonl)
              << " rules=" << (options.rules_path.empty() ? "<none>" : options.rules_path)
              << "\n";

    bool ok = server.listen(options.host, options.port);
    dashboard.stop();
    if (!ok) {
        std::cerr << "failed to bind " << options.host << ":" << options.port << "\n";
        return 1;
    }
    return 0;
}
