#ifndef BLAZERULES_COMPILER_H
#define BLAZERULES_COMPILER_H

#include "rule_spec.h"
#include "schema.h"
#include "result.h"

CompileResult compile_rule_file(const RuleFileSpec& spec, const BlazeRulesSchema& schema);

#endif //BLAZERULES_COMPILER_H
