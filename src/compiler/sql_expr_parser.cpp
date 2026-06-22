#include "blazerules/sql_expr_parser.h"

#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---- Lexer -------------------------------------------------------------------
enum class Tok { END, IDENT, NUMBER, STRING, OP, LPAREN, RPAREN, COMMA };

struct Token {
    Tok kind = Tok::END;
    std::string text;   // identifier/operator text, or raw string/number literal
    double number = 0.0;
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

uint64_t fnv1a64(std::string_view text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string sanitize_identifier(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) out.push_back(ch);
        else out.push_back('_');
    }
    if (out.empty()) out = "array";
    return out;
}

std::string array_any_synthetic_name(std::string_view path, std::string_view fingerprint) {
    std::ostringstream os;
    os << "__array_any_" << sanitize_identifier(path) << '_'
       << std::hex << fnv1a64(fingerprint);
    return os.str();
}

std::string strip_alias_prefixes(std::string body, const std::string& alias) {
    const std::string prefix = alias + ".";
    size_t pos = 0;
    while ((pos = body.find(prefix, pos)) != std::string::npos) {
        bool boundary = pos == 0 ||
            !(std::isalnum(static_cast<unsigned char>(body[pos - 1])) || body[pos - 1] == '_');
        if (boundary) body.erase(pos, prefix.size());
        else pos += prefix.size();
    }
    return body;
}

bool parse_any_match_call(const std::string& text, std::string& path, std::string& alias,
                          std::string& body, std::string& error) {
    std::string trimmed = trim(text);
    std::string upper_trimmed = upper(trimmed);
    const std::string fn = "ANY_MATCH";
    if (upper_trimmed.rfind(fn, 0) != 0) return false;
    size_t open = trimmed.find('(');
    if (open == std::string::npos || trimmed.back() != ')') {
        error = "any_match requires parentheses";
        return true;
    }
    std::string inner = trimmed.substr(open + 1, trimmed.size() - open - 2);
    int depth = 0;
    bool quoted = false;
    char quote = 0;
    size_t comma = std::string::npos;
    for (size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (quoted) {
            if (c == quote) quoted = false;
            continue;
        }
        if (c == '\'' || c == '"') {
            quoted = true;
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == ',' && depth == 0) {
            comma = i;
            break;
        }
    }
    if (comma == std::string::npos) {
        error = "any_match requires array path and lambda";
        return true;
    }
    path = trim(inner.substr(0, comma));
    std::string lambda = trim(inner.substr(comma + 1));
    size_t arrow = lambda.find("->");
    if (arrow == std::string::npos) {
        error = "any_match lambda must use ->";
        return true;
    }
    alias = trim(lambda.substr(0, arrow));
    body = trim(lambda.substr(arrow + 2));
    if (path.empty() || alias.empty() || body.empty()) {
        error = "any_match requires non-empty path, alias, and body";
    }
    return true;
}

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    Token next() {
        skip_ws();
        if (pos_ >= src_.size()) return {Tok::END, "", 0.0};
        char c = src_[pos_];
        if (c == '(') { ++pos_; return {Tok::LPAREN, "(", 0.0}; }
        if (c == ')') { ++pos_; return {Tok::RPAREN, ")", 0.0}; }
        if (c == ',') { ++pos_; return {Tok::COMMA, ",", 0.0}; }
        if (c == '\'' || c == '"') return string_literal(c);
        if (c == '>' || c == '<' || c == '=' || c == '!') return operator_token();
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
            (c == '.' && pos_ + 1 < src_.size() &&
             std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
            return number_token();
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return ident_token();
        throw std::runtime_error(std::string("unexpected character '") + c + "'");
    }

private:
    void skip_ws() {
        while (pos_ < src_.size() &&
               std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }
    Token string_literal(char quote) {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < src_.size() && src_[pos_] != quote) out.push_back(src_[pos_++]);
        if (pos_ >= src_.size()) throw std::runtime_error("unterminated string literal");
        ++pos_;  // closing quote
        return {Tok::STRING, out, 0.0};
    }
    Token operator_token() {
        std::string op(1, src_[pos_++]);
        if (pos_ < src_.size() && (src_[pos_] == '=' || (op == "<" && src_[pos_] == '>'))) {
            op.push_back(src_[pos_++]);
        }
        return {Tok::OP, op, 0.0};
    }
    Token number_token() {
        size_t start = pos_;
        if (src_[pos_] == '-' || src_[pos_] == '+') ++pos_;
        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.' ||
                src_[pos_] == 'e' || src_[pos_] == 'E' || src_[pos_] == '-' || src_[pos_] == '+')) {
            // stop a trailing sign that isn't part of an exponent
            if ((src_[pos_] == '-' || src_[pos_] == '+') &&
                !(pos_ > start && (src_[pos_ - 1] == 'e' || src_[pos_ - 1] == 'E'))) break;
            ++pos_;
        }
        std::string num = src_.substr(start, pos_ - start);
        return {Tok::NUMBER, num, std::stod(num)};
    }
    Token ident_token() {
        size_t start = pos_;
        while (pos_ < src_.size() &&
               (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_' ||
                src_[pos_] == '.')) ++pos_;
        return {Tok::IDENT, src_.substr(start, pos_ - start), 0.0};
    }

    const std::string& src_;
    size_t pos_ = 0;
};

// ---- Parser ------------------------------------------------------------------
class Parser {
public:
    explicit Parser(const std::string& src) : lexer_(src) { advance(); }

    ConditionSpec parse() {
        ConditionSpec c = parse_or();
        if (cur_.kind != Tok::END) throw std::runtime_error("trailing tokens after expression");
        return c;
    }

private:
    void advance() { cur_ = lexer_.next(); }
    bool is_keyword(const char* kw) const {
        return cur_.kind == Tok::IDENT && upper(cur_.text) == kw;
    }
    void expect(Tok kind, const char* what) {
        if (cur_.kind != kind) throw std::runtime_error(std::string("expected ") + what);
        advance();
    }

    ConditionSpec parse_or() {
        ConditionSpec left = parse_and();
        std::vector<ConditionSpec> kids;
        while (is_keyword("OR")) {
            if (kids.empty()) kids.push_back(std::move(left));
            advance();
            kids.push_back(parse_and());
        }
        if (kids.empty()) return left;
        OrConditionSpec spec;
        spec.child_condition = std::move(kids);
        return ConditionSpec(std::move(spec));
    }

    ConditionSpec parse_and() {
        ConditionSpec left = parse_not();
        std::vector<ConditionSpec> kids;
        while (is_keyword("AND")) {
            if (kids.empty()) kids.push_back(std::move(left));
            advance();
            kids.push_back(parse_not());
        }
        if (kids.empty()) return left;
        AndConditionSpec spec;
        spec.child_condition = std::move(kids);
        return ConditionSpec(std::move(spec));
    }

    ConditionSpec parse_not() {
        if (is_keyword("NOT")) {
            advance();
            NotConditionSpec spec;
            spec.child_condition.push_back(parse_not());
            return ConditionSpec(std::move(spec));
        }
        return parse_primary();
    }

    ConditionSpec parse_primary() {
        if (cur_.kind == Tok::LPAREN) {
            advance();
            ConditionSpec c = parse_or();
            expect(Tok::RPAREN, "')'");
            return c;
        }
        return parse_comparison();
    }

    // Read a single value token as a numeric (if NUMBER) or string (if STRING/IDENT).
    void read_value(bool& is_number, double& num, std::string& str) {
        if (cur_.kind == Tok::NUMBER) {
            is_number = true; num = cur_.number; advance();
        } else if (cur_.kind == Tok::STRING || cur_.kind == Tok::IDENT) {
            is_number = false; str = cur_.text; advance();
        } else {
            throw std::runtime_error("expected a value");
        }
    }

    ConditionSpec parse_comparison() {
        if (cur_.kind != Tok::IDENT) throw std::runtime_error("expected a field name");
        std::string field = cur_.text;
        advance();

        // IS [NOT] NULL
        if (is_keyword("IS")) {
            advance();
            bool neg = false;
            if (is_keyword("NOT")) { neg = true; advance(); }
            if (!is_keyword("NULL")) throw std::runtime_error("expected NULL after IS");
            advance();
            NullConditionSpec spec;
            spec.field = field;
            spec.op = neg ? OpType::IS_NOT_NULL : OpType::IS_NULL;
            return ConditionSpec(std::move(spec));
        }

        // [NOT] IN (...)
        bool not_in = false;
        if (is_keyword("NOT") ) { not_in = true; advance(); if (!is_keyword("IN")) throw std::runtime_error("expected IN after NOT"); }
        if (is_keyword("IN")) {
            advance();
            expect(Tok::LPAREN, "'(' after IN");
            std::vector<std::string> vals;
            while (cur_.kind != Tok::RPAREN) {
                bool isn; double n; std::string s;
                read_value(isn, n, s);
                vals.push_back(isn ? num_to_string(n) : s);
                if (cur_.kind == Tok::COMMA) advance();
                else break;
            }
            expect(Tok::RPAREN, "')' to close IN list");
            CategoricalConditionSpec spec;
            spec.field = field;
            spec.op = not_in ? OpType::NOT_IN : OpType::IN;
            spec.values = std::move(vals);
            return ConditionSpec(std::move(spec));
        }
        if (not_in) throw std::runtime_error("expected IN after NOT");

        // BETWEEN a AND b
        if (is_keyword("BETWEEN")) {
            advance();
            bool isn; double lo = 0, hi = 0; std::string s;
            read_value(isn, lo, s);
            if (!isn) throw std::runtime_error("BETWEEN requires numeric bounds");
            if (!is_keyword("AND")) throw std::runtime_error("expected AND in BETWEEN");
            advance();
            read_value(isn, hi, s);
            if (!isn) throw std::runtime_error("BETWEEN requires numeric bounds");
            NumericRangeConditionSpec spec;
            spec.field = field;
            spec.op = OpType::BETWEEN_INCLUDING;
            spec.lower = lo;
            spec.upper = hi;
            return ConditionSpec(std::move(spec));
        }

        // LIKE / ILIKE
        if (is_keyword("LIKE") || is_keyword("ILIKE")) {
            advance();
            if (cur_.kind != Tok::STRING) throw std::runtime_error("LIKE requires a string pattern");
            std::string pat = cur_.text;
            advance();
            return like_to_condition(field, pat);
        }

        // comparison operator
        if (cur_.kind != Tok::OP) throw std::runtime_error("expected a comparison operator");
        std::string op = cur_.text;
        advance();
        bool isn; double n = 0; std::string s;
        read_value(isn, n, s);

        if (isn) {
            NumericConditionSpec spec;
            spec.field = field;
            spec.op = numeric_op(op);
            spec.threshold = n;
            return ConditionSpec(std::move(spec));
        }
        // string/categorical comparison: only equality / inequality make sense
        CategoricalConditionSpec spec;
        spec.field = field;
        if (op == "=" || op == "==") spec.op = OpType::EQ;
        else if (op == "!=" || op == "<>") spec.op = OpType::NEQ;
        else throw std::runtime_error("operator '" + op + "' not valid for a string value");
        spec.values = std::vector<std::string>{s};
        return ConditionSpec(std::move(spec));
    }

    static std::string num_to_string(double n) {
        if (n == static_cast<double>(static_cast<long long>(n)))
            return std::to_string(static_cast<long long>(n));
        return std::to_string(n);
    }

    static OpType numeric_op(const std::string& op) {
        if (op == ">") return OpType::GT;
        if (op == "<") return OpType::LT;
        if (op == ">=") return OpType::GTE;
        if (op == "<=") return OpType::LTE;
        if (op == "=" || op == "==") return OpType::EQ;
        if (op == "!=" || op == "<>") return OpType::NEQ;
        throw std::runtime_error("unknown comparison operator '" + op + "'");
    }

    static ConditionSpec like_to_condition(const std::string& field, const std::string& pat) {
        bool lead = !pat.empty() && pat.front() == '%';
        bool trail = !pat.empty() && pat.back() == '%';
        std::string core = pat;
        if (lead) core.erase(core.begin());
        if (trail && !core.empty()) core.pop_back();
        StringConditionSpec spec;
        spec.field = field;
        spec.value = core;
        if (lead && trail) spec.op = OpType::CONTAINS;
        else if (trail) spec.op = OpType::STARTS_WITH;
        else if (lead) spec.op = OpType::ENDS_WITH;
        else spec.op = OpType::CONTAINS;  // no wildcard -> substring match
        return ConditionSpec(std::move(spec));
    }

    Lexer lexer_;
    Token cur_;
};

}  // namespace

SqlParseResult parse_sql_expression(const std::string& text) {
    SqlParseResult result;
    try {
        std::string path;
        std::string alias;
        std::string body;
        std::string any_match_error;
        if (parse_any_match_call(text, path, alias, body, any_match_error)) {
            if (!any_match_error.empty()) throw std::runtime_error(any_match_error);
            std::string synthetic_field = array_any_synthetic_name(path, text);
            std::string nested_body = strip_alias_prefixes(body, alias);
            Parser nested(nested_body);
            ArrayAnyConditionSpec spec;
            spec.path = std::move(path);
            spec.where = std::make_shared<ConditionSpec>(nested.parse());
            spec.synthetic_field = std::move(synthetic_field);
            result.condition = ConditionSpec(std::move(spec));
            result.ok = true;
            return result;
        }
        Parser parser(text);
        result.condition = parser.parse();
        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
    }
    return result;
}
