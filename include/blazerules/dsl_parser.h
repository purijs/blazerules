#ifndef BLAZERULES_DSL_PARSER_H
#define BLAZERULES_DSL_PARSER_H

#include "result.h"
#include <yaml-cpp/yaml.h>

ParseFileResult parse_rule_file(const std::string& yaml_path);
ParseFileResult parse_rule_string(const std::string& contents);

#endif //BLAZERULES_DSL_PARSER_H
