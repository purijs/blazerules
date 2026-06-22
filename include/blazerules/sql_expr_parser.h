#ifndef BLAZERULES_SQL_EXPR_PARSER_H
#define BLAZERULES_SQL_EXPR_PARSER_H

#include <string>

#include "rule_spec.h"

struct SqlParseResult {
    bool ok = false;
    ConditionSpec condition;
    std::string error;
};

SqlParseResult parse_sql_expression(const std::string& text);

#endif // BLAZERULES_SQL_EXPR_PARSER_H
