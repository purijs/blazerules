//
// Created by Jaskaran Singh Puri on 29.05.26.
//

#ifndef VDE_DSL_PARSER_H
#define VDE_DSL_PARSER_H

#include "result.h"
#include <yaml-cpp/yaml.h>

ParseFileResult parse_rule_file(const std::string& yaml_path);

#endif //VDE_DSL_PARSER_H
