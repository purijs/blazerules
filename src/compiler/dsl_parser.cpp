//
// Created by Jaskaran Singh Puri on 26.05.26.
//

#include "../../include/vde/dsl_parser.h"
#include <iostream>

std::string to_lowercase(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

static bool action_from_string(const std::string& action_str, ActionType& action) {
    if (action_str == "flag") {
        action = ActionType::FLAG;
        return true;
    } else if (action_str == "block") {
        action = ActionType::BLOCK;
        return true;
    } else if (action_str == "review") {
        action = ActionType::REVIEW;
        return true;
    } else if (action_str == "approve") {
        action = ActionType::APPROVE;
        return true;
    }
    return false;
}

static bool severity_from_string(const std::string& severity_str, severity& sev) {
    if (severity_str == "low") {
        sev = severity::LOW;
        return true;
    } else if (severity_str == "medium") {
        sev = severity::MEDIUM;
        return true;
    } else if (severity_str == "high") {
        sev = severity::HIGH;
        return true;
    } else if (severity_str == "critical") {
        sev = severity::CRITICAL;
        return true;
    }
    return false;
}

static void parse_option_from_string(const std::string& option_str, OpType& option) {
    if (option_str == "gt") {
        option = OpType::GT;
    } else if (option_str == "lt") {
        option = OpType::LT;
    } else if (option_str == "gte") {
        option = OpType::GTE;
    } else if (option_str == "lte") {
        option = OpType::LTE;
    } else if (option_str == "eq") {
        option = OpType::EQ;
    } else if (option_str == "neq") {
        option = OpType::NEQ;
    } else if (option_str == "in") {
        option = OpType::IN;
    } else if (option_str == "not_in") {
        option = OpType::NOT_IN;
    } else if (option_str == "between_excluding") {
        option = OpType::BETWEEN_EXCLUDING;
    } else if (option_str == "between_including") {
        option = OpType::BETWEEN_INCLUDING;
    } else {
        option = OpType::INVALID;
    }
}

static void parse_window_fn_from_string(const std::string& window_fn_str, WindowConditionSpec& spec) {
    if (window_fn_str == "count") {
        spec.windowfn = WindowFn::COUNT;
    } else if (window_fn_str == "sum") {
        spec.windowfn = WindowFn::SUM;
    } else if (window_fn_str == "min") {
        spec.windowfn = WindowFn::MIN;
    } else if (window_fn_str == "max") {
        spec.windowfn = WindowFn::MAX;
    }
}

static ParseConditionResult parse_leaf(const YAML::Node& node) {
    std::string dump = YAML::Dump(node);
    OpType op;

    if (node["window"]) {
        parse_option_from_string(to_lowercase(node["window"]["op"].as<std::string>()), op);

        WindowConditionSpec spec;
        spec.field = node["window"]["entity_field"].as<std::string>();
        parse_window_fn_from_string(to_lowercase(node["window"]["function"].as<std::string>()), spec);
        spec.op = op;
        spec.duration_seconds = node["window"]["duration_seconds"].as<int>();
        spec.threshold = node["window"]["value"].as<double>();

        return ParseConditionResult{.ok = true, .value = spec, .error = VdeError{}};
    } else {

        parse_option_from_string(to_lowercase(node["op"].as<std::string>()), op);

        if (node["value"].IsSequence()) {

            if (op == OpType::BETWEEN_EXCLUDING || op == OpType::BETWEEN_INCLUDING) {
                double lower = node["value"][0].as<double>();
                double upper = node["value"][1].as<double>();

                NumericRangeConditionSpec spec;
                spec.field = node["field"].as<std::string>();
                spec.op = op;
                spec.lower = lower;
                spec.upper = upper;

                return ParseConditionResult{.ok = true, .value = spec, .error = VdeError{}};
            } else {
                std::vector<std::string> values;
                for (const auto& val : node["value"]) {
                    values.push_back(val.as<std::string>());
                }

                CategoricalConditionSpec spec;
                spec.field = node["field"].as<std::string>();
                spec.op = op;
                spec.values = std::move(values);

                return ParseConditionResult{.ok = true, .value = spec, .error = VdeError{}};
            }
        } else {
            try {
                double value = node["value"].as<double>();

                NumericConditionSpec spec;
                spec.field = node["field"].as<std::string>();
                spec.op = op;
                spec.threshold = value;

                return ParseConditionResult{.ok = true, .value = spec, .error = VdeError{}};
            } catch (const YAML::BadConversion& e) {
                std::string value = node["value"].as<std::string>();

                CategoricalConditionSpec spec;
                spec.field = node["field"].as<std::string>();
                spec.op = op;
                spec.values = std::move(value);

                return ParseConditionResult{.ok = true, .value = spec, .error = VdeError{}};
            }
        }
    }

}

static ParseConditionResult parse_condition(const YAML::Node& node, const std::string& rule_id) {
    if (node["and"]) {
        AndConditionSpec and_spec;
        for (const auto& child_node : node["and"]) {
            ParseConditionResult child_result = parse_condition(child_node, rule_id);
            and_spec.child_condition.push_back(child_result.value);
        }

        return ParseConditionResult{.ok = true, .value = and_spec, .error = VdeError{}};
    } else if (node["or"]) {
        OrConditionSpec or_spec;
        for (const auto& child_node : node["or"]) {
            ParseConditionResult child_result = parse_condition(child_node, rule_id);
            or_spec.child_condition.push_back(child_result.value);
        }

        return ParseConditionResult{.ok = true, .value = or_spec, .error = VdeError{}};
    } else {
        return parse_leaf(node);
    }
}

static ParseRuleResult parse_rule(const YAML::Node& node) {
    RuleSpec spec;
    spec.id = node["id"].as<std::string>();

    std::string action_as_string = to_lowercase(node["action"].as<std::string>());
    if (!action_from_string(action_as_string, spec.action)) {
        VdeError err;
        err.code = ErrorCode::UNKNOWN_ACTION;
        err.message = "Unknown action: " + action_as_string;
        err.rule_id = spec.id;
        return ParseRuleResult{.ok = false, .value = RuleSpec{}, .error = err};
    }

    std::string severity_as_string = to_lowercase(node["severity"].as<std::string>());
    if (!severity_from_string(severity_as_string, spec.severity)) {
        VdeError err;
        err.code = ErrorCode::UNKNOWN_SEVERITY;
        err.message = "Unknown severity: " + severity_as_string;
        err.rule_id = spec.id;
        return ParseRuleResult{.ok = false, .value = RuleSpec{}, .error = err};
    }

    ParseConditionResult parsed_rule = parse_condition(node["conditions"], spec.id);
    spec.root_condition = parsed_rule.value;

    std::cout << "Parsed Rule: " << spec.id << std::endl;
    return ParseRuleResult{.ok = true, .value = spec, .error = VdeError{}};
}

ParseFileResult parse_rule_file(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    RuleFileSpec spec;

    for (const auto& rule : root["ruleset"]["rules"]) {
        spec.rules.push_back(parse_rule(rule).value);
    }

    spec.name = root["ruleset"]["name"].as<std::string>();
    spec.version = root["ruleset"]["version"].as<std::string>();

    return ParseFileResult{.ok = true, .value = spec, .error = VdeError{}};
}