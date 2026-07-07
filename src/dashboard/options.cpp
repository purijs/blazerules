#include "options.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") opt.host = need("--host");
        else if (a == "--port") opt.port = std::atoi(need("--port").c_str());
        else if (a == "--poll-ms") opt.poll_ms = std::atoi(need("--poll-ms").c_str());
        else if (a == "--tail-lines") opt.tail_lines = static_cast<size_t>(std::max(1, std::atoi(need("--tail-lines").c_str())));
        else if (a == "--decision-log") opt.decision_log = need("--decision-log");
        else if (a == "--dead-letter-log") opt.dead_letter_log = need("--dead-letter-log");
        else if (a == "--metrics-url") opt.metrics_url = need("--metrics-url");
        else if (a == "--results-jsonl") opt.results_jsonl = need("--results-jsonl");
        else if (a == "--rules") opt.rules_path = need("--rules");
        else if (a == "--candidate-rules") opt.candidate_rules_path = need("--candidate-rules");
        else if (a == "--rules-history-dir") opt.rules_history_dir = need("--rules-history-dir");
        else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: blazerules_dashboard [options]\n\n"
                << "Options:\n"
                << "  --host HOST                 default 127.0.0.1\n"
                << "  --port PORT                 default 9470\n"
                << "  --poll-ms MS                default 1000\n"
                << "  --tail-lines N              default 5000\n"
                << "  --decision-log PATH         compact decision NDJSON\n"
                << "  --dead-letter-log PATH      compact dead-letter NDJSON\n"
                << "  --metrics-url URL           Prometheus URL, e.g. http://127.0.0.1:9464/metrics\n"
                << "  --results-jsonl PATH        stress_matrix JSONL\n"
                << "  --rules PATH                active rules YAML for visualizer\n"
                << "  --candidate-rules PATH      candidate rules YAML for validation/diff\n"
                << "  --rules-history-dir DIR     directory of YAML versions\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            std::exit(2);
        }
    }
    if (opt.port <= 0) opt.port = 9470;
    if (opt.poll_ms <= 0) opt.poll_ms = 1000;
    return opt;
}
