#include "blazerules_io/cdc.h"

#include <sstream>

#include <simdjson.h>

namespace blazerules_io {

namespace {

std::string to_json(const simdjson::dom::element& el) {
    std::ostringstream oss;
    oss << el;
    return oss.str();
}

}  // namespace

std::string unwrap_debezium(const std::vector<std::string_view>& messages,
                            const std::string& op_field) {
    simdjson::dom::parser parser;
    std::string out;

    for (std::string_view msg : messages) {
        if (msg.empty()) continue;

        simdjson::dom::element doc;
        if (parser.parse(msg.data(), msg.size()).get(doc)) continue;
        if (doc.type() != simdjson::dom::element_type::OBJECT) continue;

        std::string_view op_sv;
        const bool has_op = !doc["op"].get_string().get(op_sv);
        const bool has_after = !doc["after"].error();
        const bool has_before = !doc["before"].error();

        if (!has_op && !has_after && !has_before) {
            out += to_json(doc);
            out += '\n';
            continue;
        }

        const std::string op_str = has_op ? std::string(op_sv) : "u";
        const char* state_key = (op_str == "d") ? "before" : "after";
        simdjson::dom::element state;
        if (doc[state_key].get(state) ||
            state.type() != simdjson::dom::element_type::OBJECT) {
            continue;
        }

        const std::string body = to_json(state);
        std::string line;
        line.reserve(body.size() + op_field.size() + 16);
        line += "{\"";
        line += op_field;
        line += "\":\"";
        line += op_str;
        line += '"';
        size_t i = 1;
        while (i < body.size() &&
               (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) {
            ++i;
        }
        if (i < body.size() && body[i] != '}') {
            line += ',';
            line.append(body, i, std::string::npos);
        } else {
            line += '}';
        }
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace blazerules_io
