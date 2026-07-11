#include <iostream>

#include <httplib.h>

#include "blazerules/resource_resolver.h"
#include "dashboard_server.h"
#include "options.h"
#include "routes.h"

int main(int argc, char** argv) {
    Options options = parse_args(argc, argv);
    if (options.host == "0.0.0.0") {
        std::cerr << "warning: dashboard has no authentication and is bound to 0.0.0.0\n";
    }
    if (!options.aws_region.empty()) blazerules::set_aws_region(options.aws_region);
    if (!options.aws_endpoint_url.empty()) blazerules::set_aws_endpoint_url(options.aws_endpoint_url);

    DashboardServer dashboard(options);
    dashboard.start();

    httplib::Server server;
    register_routes(server, dashboard);

    std::cout << "BlazeRules dashboard listening on http://" << options.host << ":" << options.port << "\n";
    const std::string decision_src = !options.decision_log_dir.empty() ? options.decision_log_dir
                                    : (options.decision_log.empty() ? "<none>" : options.decision_log);
    const std::string rules_src = !options.rules_dir.empty() ? options.rules_dir
                                 : (options.rules_path.empty() ? "<none>" : options.rules_path);
    std::cout << "Sources: decision_log=" << decision_src
              << " dead_letter=" << (!options.dead_letter_log.empty() ? options.dead_letter_log
                                     : (!options.decision_log_dir.empty() ? "<auto from dir>" : "<none>"))
              << " metrics_url=" << (options.metrics_url.empty() ? "<none>" : options.metrics_url)
              << " results_jsonl=" << (options.results_jsonl.empty() ? "<none>" : options.results_jsonl)
              << " rules=" << rules_src
              << "\n";

    bool ok = server.listen(options.host, options.port);
    dashboard.stop();
    if (!ok) {
        std::cerr << "failed to bind " << options.host << ":" << options.port << "\n";
        return 1;
    }
    return 0;
}
