#include "../../include/vde/dsl_parser.h"

int main() {
    std::string path = "/Users/jaskaransinghpuri/Documents/c++/vde/rules.yaml";
    ParseFileResult result = parse_rule_file(path);

    return 0;
}