#include "blazerules/dsl_parser.h"
#include "blazerules/compiler.h"
#include "blazerules/conflict.h"
#include "blazerules/simd_kernels.h"
#include <iostream>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

FieldSpec field_from_hint(const FieldHintSpec& hint) {
    FieldSpec f;
    f.name = hint.name;
    f.type = hint.has_type ? hint.type : ColumnType::STRING;
    f.nullable = hint.has_nullable ? hint.nullable : true;
    f.is_entity_field = hint.is_entity_field || f.type == ColumnType::ENTITY_KEY;
    f.closed_values = hint.values;
    return f;
}

void add_synthetic_field(const std::string& name, std::vector<FieldSpec>& fields,
                         std::unordered_set<std::string>& seen) {
    if (name.empty() || seen.contains(name)) return;
    FieldSpec f;
    f.name = name;
    f.type = ColumnType::INT32;
    f.nullable = false;
    fields.push_back(std::move(f));
    seen.insert(name);
}

void collect_synthetic_fields(const ConditionSpec& condition, std::vector<FieldSpec>& fields,
                              std::unordered_set<std::string>& seen) {
    if (const auto* arr = std::get_if<ArrayAnyConditionSpec>(&condition.node)) {
        add_synthetic_field(arr->synthetic_field, fields, seen);
        return;
    }
    if (const auto* and_node = std::get_if<AndConditionSpec>(&condition.node)) {
        for (const auto& child : and_node->child_condition) collect_synthetic_fields(child, fields, seen);
        return;
    }
    if (const auto* or_node = std::get_if<OrConditionSpec>(&condition.node)) {
        for (const auto& child : or_node->child_condition) collect_synthetic_fields(child, fields, seen);
        return;
    }
    if (const auto* not_node = std::get_if<NotConditionSpec>(&condition.node)) {
        for (const auto& child : not_node->child_condition) collect_synthetic_fields(child, fields, seen);
    }
}

BlazeRulesSchema schema_from_hints(const RuleFileSpec& spec) {
    std::vector<FieldSpec> fields;
    std::unordered_set<std::string> seen;
    fields.reserve(spec.field_hints.size() + spec.rules.size());
    for (const auto& hint : spec.field_hints) {
        if (seen.contains(hint.name)) continue;
        fields.push_back(field_from_hint(hint));
        seen.insert(hint.name);
    }
    for (const auto& rule : spec.rules) collect_synthetic_fields(rule.root_condition, fields, seen);
    return BlazeRulesSchema(std::move(fields));
}

void print_compile_report(const CompiledRuleSet& compiled) {
    std::cout << "Compiled " << compiled.rules.size() << " rules, "
              << compiled.window_channels.size() << " window channels\n";
    for (const auto& r : compiled.rules) {
        std::cout << "  rule '" << r.rule_id << "': " << r.op.size()
                  << " ops, " << r.register_count << " registers\n";
    }
}

void print_simd() {
    std::cout << "SIMD backend: " << simd_backend_name()
              << " (" << (simd_enabled() ? "vectorized" : "scalar/sse2-safe") << ")\n";
    std::cout << "CPU features: " << cpu_features_summary() << "\n";
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: blazerules_driver rules.yaml\n";
        return 2;
    }
    std::string path = argv[1];

    ParseFileResult parsed = parse_rule_file(path);
    if (!parsed.ok) {
        std::cerr << "Parse failed: " << parsed.error.message << "\n";
        return 1;
    }
    std::cout << "Parsed ruleset '" << parsed.value.name << "' v" << parsed.value.version
              << " with " << parsed.value.rules.size() << " rules\n";

    if (!parsed.value.field_hints.empty()) {
        CompileResult compiled = compile_rule_file(parsed.value, schema_from_hints(parsed.value));
        if (compiled.ok) {
            print_compile_report(compiled.value);
        } else {
            std::cout << "Compile skipped: " << compiled.error.message << "\n";
        }
    } else {
        std::cout << "Compile skipped: no field hints in rules file\n";
    }

    ConflictReport report = analyze_conflicts(parsed.value);
    std::cout << report.to_string() << "\n";
    print_simd();
    return 0;
}
