#include "blazerules/compiler.h"

#include <chrono>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#include "blazerules/resource_resolver.h"

namespace {

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

OpType invert_op(OpType op) {
    switch (op) {
        case OpType::GT: return OpType::LTE;
        case OpType::GTE: return OpType::LT;
        case OpType::LT: return OpType::GTE;
        case OpType::LTE: return OpType::GT;
        case OpType::EQ: return OpType::NEQ;
        case OpType::NEQ: return OpType::EQ;
        case OpType::IN: return OpType::NOT_IN;
        case OpType::NOT_IN: return OpType::IN;
        case OpType::IS_NULL: return OpType::IS_NOT_NULL;
        case OpType::IS_NOT_NULL: return OpType::IS_NULL;
        case OpType::IS_EMPTY: return OpType::IS_NOT_EMPTY;
        case OpType::IS_NOT_EMPTY: return OpType::IS_EMPTY;
        case OpType::GT_FIELD: return OpType::LTE_FIELD;
        case OpType::GTE_FIELD: return OpType::LT_FIELD;
        case OpType::LT_FIELD: return OpType::GTE_FIELD;
        case OpType::LTE_FIELD: return OpType::GT_FIELD;
        case OpType::EQ_FIELD: return OpType::NEQ_FIELD;
        case OpType::NEQ_FIELD: return OpType::EQ_FIELD;
        case OpType::CONTAINS_ANY: return OpType::NOT_INTERSECTS;
        case OpType::INTERSECTS: return OpType::NOT_INTERSECTS;
        case OpType::NOT_INTERSECTS: return OpType::INTERSECTS;
        case OpType::FLAGS_ANY: return OpType::FLAGS_NONE;
        case OpType::FLAGS_NONE: return OpType::FLAGS_ANY;
        case OpType::IP_IN_SUBNET: return OpType::IP_NOT_IN_SUBNET;
        case OpType::IP_NOT_IN_SUBNET: return OpType::IP_IN_SUBNET;
        case OpType::REGEX: return OpType::NOT_REGEX;
        case OpType::NOT_REGEX: return OpType::REGEX;
        case OpType::IN_LOOKUP: return OpType::NOT_IN_LOOKUP;
        case OpType::NOT_IN_LOOKUP: return OpType::IN_LOOKUP;
        default: return op;
    }
}

bool parse_ipv4(std::string_view s, uint32_t& out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    uint32_t value = 0;
    bool saw_digit = false;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') {
            value = value * 10u + static_cast<uint32_t>(ch - '0');
            saw_digit = true;
            if (value > 255u) return false;
        } else if (ch == '.' && saw_digit && part < 3) {
            parts[part++] = value;
            value = 0;
            saw_digit = false;
        } else {
            return false;
        }
    }
    if (!saw_digit || part != 3) return false;
    parts[part] = value;
    out = (parts[0] << 24u) | (parts[1] << 16u) | (parts[2] << 8u) | parts[3];
    return true;
}

void parse_cidr(std::string_view cidr, uint32_t& network, uint32_t& mask) {
    size_t slash = cidr.find('/');
    std::string_view ip = slash == std::string_view::npos ? cidr : cidr.substr(0, slash);
    int prefix = slash == std::string_view::npos
        ? 32
        : std::max(0, std::min(32, std::stoi(std::string(cidr.substr(slash + 1)))));
    uint32_t raw = 0;
    (void)parse_ipv4(ip, raw);
    mask = prefix == 0 ? 0u : (0xffffffffu << (32 - prefix));
    network = raw & mask;
}

void prepare_nested_condition(ConditionSpec& condition) {
    std::visit([&](auto& spec) {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, RegexConditionSpec>) {
            if (!spec.compiled) spec.compiled = std::make_shared<RE2>(spec.pattern);
        } else if constexpr (std::is_same_v<T, CidrConditionSpec>) {
            parse_cidr(spec.cidr, spec.network, spec.mask);
            spec.compiled = true;
        } else if constexpr (std::is_same_v<T, ArrayAnyConditionSpec>) {
            if (spec.where) prepare_nested_condition(*spec.where);
        } else if constexpr (std::is_same_v<T, AndConditionSpec> ||
                             std::is_same_v<T, OrConditionSpec> ||
                             std::is_same_v<T, NotConditionSpec>) {
            for (auto& child : spec.child_condition) prepare_nested_condition(child);
        }
    }, condition.node);
}

ValueExprKind convert_expr_kind(ArithmeticExprKind kind) {
    switch (kind) {
        case ArithmeticExprKind::LITERAL: return ValueExprKind::LITERAL;
        case ArithmeticExprKind::ADD: return ValueExprKind::ADD;
        case ArithmeticExprKind::SUB: return ValueExprKind::SUB;
        case ArithmeticExprKind::MUL: return ValueExprKind::MUL;
        case ArithmeticExprKind::DIV: return ValueExprKind::DIV;
        case ArithmeticExprKind::FIELD:
        default: return ValueExprKind::FIELD;
    }
}

LookupSetType lookup_type_from_string(std::string_view raw) {
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "int_set") return LookupSetType::INT_SET;
    if (value == "ipv4_cidr_set") return LookupSetType::IPV4_CIDR_SET;
    return LookupSetType::STRING_SET;
}

std::string trim(std::string value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    value = value.substr(first, last - first);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> cells;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (c == ',' && !quoted) {
            cells.push_back(trim(std::move(current)));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    cells.push_back(trim(std::move(current)));
    return cells;
}

std::filesystem::path resolve_lookup_path(const RuleFileSpec& spec, const LookupSpec& lookup) {
    std::string logical = blazerules::join_resource_path(spec.base_dir, lookup.path);
    return std::filesystem::path(blazerules::resolve_resource_to_local(logical));
}

void add_cidr_range(std::string_view cidr, std::vector<Ipv4Range>& ranges) {
    uint32_t network = 0;
    uint32_t mask = 0;
    parse_cidr(cidr, network, mask);
    uint32_t end = network | ~mask;
    ranges.push_back(Ipv4Range{network, end});
}

void normalize_ranges(std::vector<Ipv4Range>& ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const Ipv4Range& a, const Ipv4Range& b) {
        return a.start < b.start || (a.start == b.start && a.end < b.end);
    });
    std::vector<Ipv4Range> merged;
    for (const auto& r : ranges) {
        if (merged.empty() || r.start > merged.back().end + 1u) {
            merged.push_back(r);
        } else {
            merged.back().end = std::max(merged.back().end, r.end);
        }
    }
    ranges.swap(merged);
}

BlazeRulesResult<std::shared_ptr<const CompiledLookupSet>> load_lookup_set(
        const RuleFileSpec& spec,
        const LookupSpec& lookup,
        uint64_t generation) {
    auto compiled = std::make_shared<CompiledLookupSet>();
    compiled->name = lookup.name;
    compiled->type = lookup_type_from_string(lookup.type);
    compiled->generation = generation;

    std::filesystem::path lookup_path;
    try {
        lookup_path = resolve_lookup_path(spec, lookup);
    } catch (const std::exception& e) {
        return BlazeRulesResult<std::shared_ptr<const CompiledLookupSet>>::err(
            {BlazeRulesError::MISSING_REQUIRED_FIELD, e.what(),
             "lookup", lookup.name, -1, BlazeRulesError::Domain::LOOKUP});
    }
    std::ifstream in(lookup_path);
    if (!in) {
        return BlazeRulesResult<std::shared_ptr<const CompiledLookupSet>>::err(
            {BlazeRulesError::MISSING_REQUIRED_FIELD, "lookup file not found: " + lookup.path,
             "lookup", lookup.name, -1, BlazeRulesError::Domain::LOOKUP});
    }

    std::string header_line;
    if (!std::getline(in, header_line)) {
        return BlazeRulesResult<std::shared_ptr<const CompiledLookupSet>>::err(
            {BlazeRulesError::MISSING_REQUIRED_FIELD, "lookup file is empty: " + lookup.path,
             "lookup", lookup.name, -1, BlazeRulesError::Domain::LOOKUP});
    }
    auto header = split_csv_line(header_line);
    std::string wanted = compiled->type == LookupSetType::IPV4_CIDR_SET ? "cidr" : "value";
    int value_col = -1;
    for (int i = 0; i < static_cast<int>(header.size()); ++i) {
        std::string h = header[static_cast<size_t>(i)];
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (h == wanted) {
            value_col = i;
            break;
        }
    }
    if (value_col < 0) value_col = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto cells = split_csv_line(line);
        if (value_col >= static_cast<int>(cells.size())) continue;
        const std::string& value = cells[static_cast<size_t>(value_col)];
        if (value.empty()) continue;
        if (compiled->type == LookupSetType::STRING_SET) {
            compiled->strings.push_back(value);
        } else if (compiled->type == LookupSetType::INT_SET) {
            compiled->ints.push_back(std::stoll(value));
        } else {
            add_cidr_range(value, compiled->ipv4_ranges);
        }
    }

    if (compiled->type == LookupSetType::STRING_SET) {
        std::sort(compiled->strings.begin(), compiled->strings.end());
        compiled->strings.erase(std::unique(compiled->strings.begin(), compiled->strings.end()),
                                compiled->strings.end());
    } else if (compiled->type == LookupSetType::INT_SET) {
        std::sort(compiled->ints.begin(), compiled->ints.end());
        compiled->ints.erase(std::unique(compiled->ints.begin(), compiled->ints.end()),
                             compiled->ints.end());
    } else {
        normalize_ranges(compiled->ipv4_ranges);
    }

    return BlazeRulesResult<std::shared_ptr<const CompiledLookupSet>>::ok(std::move(compiled));
}

BlazeRulesResult<LookupRegistry> load_lookup_registry(const RuleFileSpec& spec) {
    LookupRegistry registry;
    uint64_t generation = 1;
    for (const auto& lookup : spec.lookups) {
        auto loaded = load_lookup_set(spec, lookup, generation++);
        if (loaded.is_error()) return BlazeRulesResult<LookupRegistry>::err(loaded.error());
        registry.emplace(lookup.name, loaded.value());
    }
    return BlazeRulesResult<LookupRegistry>::ok(std::move(registry));
}

struct CompileCtx {
    const BlazeRulesSchema& schema;
    std::vector<KernelOp>& seq;
    int& next_register;
    std::vector<WindowChannelSpec>& channels;
    std::vector<ModelChannelSpec>& model_channels;
    std::vector<VectorChannelSpec>& vector_channels;
    std::vector<ArrayAnyChannelSpec>& array_any_channels;
    DerivedColumnPlan& derived_plan;
    const LookupRegistry& lookups;
};

// All derived-column producers (windows, ML scores, vector distances, time-series
// aggregates) allocate their injected column index here from one shared counter, so
// indices never collide. The injected index is appended after the bound schema's
// fields: num_fields() + (number of derived columns already allocated).
int allocate_derived_column(CompileCtx& ctx, DerivedColumnKind kind, int producer_index,
                            std::string name, ColumnType type) {
    int idx = ctx.schema.num_fields() + ctx.derived_plan.count();
    ctx.derived_plan.slots.push_back(
        DerivedColumnSlot{kind, producer_index, idx, std::move(name), type});
    return idx;
}

int register_window_channel(CompileCtx& ctx, const WindowConditionSpec& c) {
    int entity_col = ctx.schema.index_of(c.field);
    int sum_col = -1;
    int denominator_col = -1;
    if ((c.windowfn == WindowFn::SUM || c.windowfn == WindowFn::AVG ||
         c.windowfn == WindowFn::RATIO || c.windowfn == WindowFn::MIN ||
         c.windowfn == WindowFn::MAX) && !c.sum_field.empty()) {
        sum_col = ctx.schema.index_of(c.sum_field);
    }
    if (c.windowfn == WindowFn::RATIO && !c.denominator_field.empty()) {
        denominator_col = ctx.schema.index_of(c.denominator_field);
    }

    for (const auto& ch : ctx.channels) {
        if (ch.entity_col_index == entity_col && ch.function == c.windowfn &&
            ch.sum_col_index == sum_col && ch.denominator_col_index == denominator_col &&
            ch.duration_seconds == c.duration_seconds) {
            return ch.injected_col_index;
        }
    }

    WindowChannelSpec ch;
    ch.entity_col_index = entity_col;
    ch.entity_field = c.field;
    ch.function = c.windowfn;
    ch.sum_col_index = sum_col;
    ch.sum_field = c.sum_field;
    ch.denominator_col_index = denominator_col;
    ch.denominator_field = c.denominator_field;
    ch.duration_seconds = c.duration_seconds;
    // COUNT yields an integer; SUM/AVG/RATIO and the MIN/MAX value-extremes are float.
    ch.column_type = (c.windowfn == WindowFn::COUNT) ? ColumnType::INT32 : ColumnType::FLOAT32;
    std::string fn_name = "count";
    if (c.windowfn == WindowFn::SUM) fn_name = "sum";
    else if (c.windowfn == WindowFn::AVG) fn_name = "avg";
    else if (c.windowfn == WindowFn::RATIO) fn_name = "ratio";
    else if (c.windowfn == WindowFn::MIN) fn_name = "min";
    else if (c.windowfn == WindowFn::MAX) fn_name = "max";
    ch.injected_name = "__window_" + c.field + "_" + fn_name + "_" +
        std::to_string(c.duration_seconds);
    // Allocate the injected column index from the shared derived-column counter.
    // For window-only rulesets this equals num_fields() + channels.size() (unchanged).
    ch.injected_col_index = allocate_derived_column(
        ctx, DerivedColumnKind::WINDOW, static_cast<int>(ctx.channels.size()),
        ch.injected_name, ch.column_type);
    ctx.channels.push_back(std::move(ch));
    return ctx.channels.back().injected_col_index;
}

int register_model_channel(CompileCtx& ctx, const ModelScoreConditionSpec& c) {
    std::vector<int> feat_idx;
    std::vector<ColumnType> feat_types;
    feat_idx.reserve(c.features.size());
    feat_types.reserve(c.features.size());
    for (const auto& f : c.features) {
        int idx = ctx.schema.index_of(f);
        feat_idx.push_back(idx);
        feat_types.push_back(ctx.schema.type_of(idx));
    }
    // Dedup: one inference pass even if many rules threshold the same model+features.
    for (const auto& ch : ctx.model_channels) {
        if (ch.model_name == c.model_name && ch.output_index == c.output_index &&
            ch.feature_col_indices == feat_idx) {
            return ch.injected_col_index;
        }
    }
    ModelChannelSpec ch;
    ch.model_name = c.model_name;
    ch.feature_col_indices = std::move(feat_idx);
    ch.feature_types = std::move(feat_types);
    ch.output_index = c.output_index;
    ch.column_type = ColumnType::FLOAT32;
    ch.injected_name = "__model_" + c.model_name + "_" + std::to_string(c.output_index) +
        "_" + std::to_string(ctx.model_channels.size());
    ch.injected_col_index = allocate_derived_column(
        ctx, DerivedColumnKind::MODEL_SCORE, static_cast<int>(ctx.model_channels.size()),
        ch.injected_name, ch.column_type);
    ctx.model_channels.push_back(std::move(ch));
    return ctx.model_channels.back().injected_col_index;
}

int register_vector_channel(CompileCtx& ctx, const VectorDistanceConditionSpec& c) {
    std::vector<int> dim_idx;
    dim_idx.reserve(c.dims.size());
    for (const auto& f : c.dims) dim_idx.push_back(ctx.schema.index_of(f));
    for (const auto& ch : ctx.vector_channels) {
        if (ch.metric == static_cast<int>(c.metric) && ch.dim_col_indices == dim_idx &&
            ch.reference == c.reference) {
            return ch.injected_col_index;
        }
    }
    VectorChannelSpec ch;
    ch.dim_col_indices = std::move(dim_idx);
    ch.reference = c.reference;
    ch.metric = static_cast<int>(c.metric);
    float norm_sq = 0.0f;
    for (float x : ch.reference) norm_sq += x * x;
    ch.reference_inv_norm = norm_sq > 0.0f ? 1.0f / std::sqrt(norm_sq) : 0.0f;
    ch.column_type = ColumnType::FLOAT32;
    const char* mname = c.metric == VectorMetric::COSINE ? "cos"
                      : (c.metric == VectorMetric::L2 ? "l2" : "dot");
    ch.injected_name = std::string("__vec_") + mname + "_" +
        std::to_string(ctx.vector_channels.size());
    ch.injected_col_index = allocate_derived_column(
        ctx, DerivedColumnKind::VECTOR_DISTANCE, static_cast<int>(ctx.vector_channels.size()),
        ch.injected_name, ch.column_type);
    ctx.vector_channels.push_back(std::move(ch));
    return ctx.vector_channels.back().injected_col_index;
}

int register_array_any_channel(CompileCtx& ctx, const ArrayAnyConditionSpec& c) {
    int synthetic_col = ctx.schema.index_of(c.synthetic_field);
    for (const auto& ch : ctx.array_any_channels) {
        if (ch.synthetic_col_index == synthetic_col) return ch.synthetic_col_index;
    }
    ArrayAnyChannelSpec ch;
    ch.path = c.path;
    if (c.where) {
        ch.where = *c.where;
        prepare_nested_condition(ch.where);
    }
    ch.synthetic_col_index = synthetic_col;
    ch.synthetic_name = c.synthetic_field;
    ctx.array_any_channels.push_back(std::move(ch));
    return synthetic_col;
}

struct CompileVisitor {
    CompileCtx& ctx;

    int compile(const ConditionSpec& c) { return std::visit(*this, c.node); }

    int compile_value_expr(const ArithmeticExprSpec& spec, std::vector<ValueExprNode>& nodes) {
        ValueExprNode node;
        node.kind = convert_expr_kind(spec.kind);
        if (spec.kind == ArithmeticExprKind::FIELD) {
            node.column_index = ctx.schema.index_of(spec.field);
            node.column_type = ctx.schema.type_of(node.column_index);
        } else if (spec.kind == ArithmeticExprKind::LITERAL) {
            node.literal = spec.literal;
        } else if (spec.children.size() >= 2) {
            node.left = compile_value_expr(spec.children[0], nodes);
            node.right = compile_value_expr(spec.children[1], nodes);
        }
        int index = static_cast<int>(nodes.size());
        nodes.push_back(std::move(node));
        return index;
    }

    int operator()(const NumericConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(NumericPredicateOp{col, reg, ctx.schema.type_of(col), c.op, c.threshold});
        return reg;
    }

    int operator()(const NumericRangeConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(NumericRangePredicateOp{col, reg, ctx.schema.type_of(col), c.op, c.lower, c.upper});
        return reg;
    }

    int operator()(const CategoricalConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(CategoricalPredicateOp{col, reg, c.op, c.values});
        return reg;
    }

    int operator()(const ArrayLenConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(ArrayLenPredicateOp{col, reg, c.op, c.length});
        return reg;
    }

    int operator()(const NullConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(NullPredicateOp{col, reg, c.op});
        return reg;
    }

    int operator()(const CrossFieldConditionSpec& c) {
        int reg = ctx.next_register++;
        int left = ctx.schema.index_of(c.field);
        int right = ctx.schema.index_of(c.other_field);
        ctx.seq.push_back(CrossFieldPredicateOp{
            left, right, reg, ctx.schema.type_of(left), ctx.schema.type_of(right), c.op});
        return reg;
    }

    int operator()(const BitfieldConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(BitfieldPredicateOp{col, reg, ctx.schema.type_of(col), c.op, c.mask});
        return reg;
    }

    int operator()(const StringConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(StringPredicateOp{col, reg, c.op, c.value, c.length});
        return reg;
    }

    int operator()(const RegexConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        auto regex = std::make_shared<RE2>(c.pattern);
        ctx.seq.push_back(RegexPredicateOp{col, reg, c.op, c.pattern, std::move(regex)});
        return reg;
    }

    int operator()(const LookupConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        auto it = ctx.lookups.find(c.lookup_name);
        std::shared_ptr<const CompiledLookupSet> lookup;
        if (it != ctx.lookups.end()) lookup = it->second;
        ctx.seq.push_back(LookupPredicateOp{col, reg, ctx.schema.type_of(col), c.op, c.lookup_name, std::move(lookup)});
        return reg;
    }

    int operator()(const CidrConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        uint32_t network = 0;
        uint32_t mask = 0;
        parse_cidr(c.cidr, network, mask);
        ctx.seq.push_back(CidrPredicateOp{col, reg, ctx.schema.type_of(col), c.op, network, mask});
        return reg;
    }

    int operator()(const TemporalConditionSpec& c) {
        int reg = ctx.next_register++;
        int col = ctx.schema.index_of(c.field);
        ctx.seq.push_back(TemporalPredicateOp{col, reg, c.op, c.value, c.values, c.lower, c.upper});
        return reg;
    }

    int operator()(const GeoDistanceConditionSpec& c) {
        int reg = ctx.next_register++;
        ctx.seq.push_back(GeoDistancePredicateOp{
            ctx.schema.index_of(c.lat_field),
            ctx.schema.index_of(c.lon_field),
            ctx.schema.index_of(c.other_lat_field),
            ctx.schema.index_of(c.other_lon_field),
            reg,
            c.op,
            c.threshold_km});
        return reg;
    }

    int operator()(const ArithmeticConditionSpec& c) {
        int reg = ctx.next_register++;
        ArithmeticPredicateOp op;
        op.output_register = reg;
        op.op_type = c.op;
        op.threshold = c.threshold;
        op.root_value = compile_value_expr(c.expr, op.value_nodes);
        if (!c.other_field.empty()) {
            op.other_column_index = ctx.schema.index_of(c.other_field);
            op.other_column_type = ctx.schema.type_of(op.other_column_index);
        }
        ctx.seq.push_back(std::move(op));
        return reg;
    }

    int operator()(const WindowConditionSpec& c) {
        int reg = ctx.next_register++;
        int injected = register_window_channel(ctx, c);
        // Must match the injected column type set in register_window_channel:
        // COUNT -> INT32, everything else (SUM/AVG/RATIO/MIN/MAX) -> FLOAT32.
        ColumnType ct = (c.windowfn == WindowFn::COUNT) ? ColumnType::INT32 : ColumnType::FLOAT32;
        ctx.seq.push_back(WindowPredicateOp{injected, reg, ct, c.op, c.threshold});
        return reg;
    }

    int operator()(const ModelScoreConditionSpec& c) {
        int reg = ctx.next_register++;
        int injected = register_model_channel(ctx, c);
        // The score is an injected FLOAT32 column; compare it with an ordinary numeric op
        // (identical hot-path kernel to any other numeric predicate).
        if (c.op == OpType::BETWEEN_INCLUDING || c.op == OpType::BETWEEN_EXCLUDING) {
            ctx.seq.push_back(NumericRangePredicateOp{injected, reg, ColumnType::FLOAT32,
                                                      c.op, c.lower, c.upper});
        } else {
            ctx.seq.push_back(NumericPredicateOp{injected, reg, ColumnType::FLOAT32,
                                                 c.op, c.threshold});
        }
        return reg;
    }

    int operator()(const VectorDistanceConditionSpec& c) {
        int reg = ctx.next_register++;
        int injected = register_vector_channel(ctx, c);
        if (c.op == OpType::BETWEEN_INCLUDING || c.op == OpType::BETWEEN_EXCLUDING) {
            ctx.seq.push_back(NumericRangePredicateOp{injected, reg, ColumnType::FLOAT32,
                                                      c.op, c.lower, c.upper});
        } else {
            ctx.seq.push_back(NumericPredicateOp{injected, reg, ColumnType::FLOAT32,
                                                 c.op, c.threshold});
        }
        return reg;
    }

    int operator()(const ArrayAnyConditionSpec& c) {
        int reg = ctx.next_register++;
        int synthetic = register_array_any_channel(ctx, c);
        ctx.seq.push_back(NumericPredicateOp{synthetic, reg, ctx.schema.type_of(synthetic),
                                             OpType::GT, 0.0});
        return reg;
    }

    int operator()(const AndConditionSpec& c) {
        std::vector<int> child_regs;
        child_regs.reserve(c.child_condition.size());
        for (const auto& child : c.child_condition) child_regs.push_back(compile(child));
        int reg = ctx.next_register++;
        ctx.seq.push_back(BitwiseAndOp{std::move(child_regs), reg});
        return reg;
    }

    int operator()(const OrConditionSpec& c) {
        std::vector<int> child_regs;
        child_regs.reserve(c.child_condition.size());
        for (const auto& child : c.child_condition) child_regs.push_back(compile(child));
        int reg = ctx.next_register++;
        ctx.seq.push_back(BitwiseOrOp{std::move(child_regs), reg});
        return reg;
    }

    int operator()(const NotConditionSpec& c) {
        const ConditionSpec& child = c.child_condition.front();

        if (const auto* n = std::get_if<NumericConditionSpec>(&child.node)) {
            NumericConditionSpec copy = *n;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* cat = std::get_if<CategoricalConditionSpec>(&child.node)) {
            CategoricalConditionSpec copy = *cat;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* null = std::get_if<NullConditionSpec>(&child.node)) {
            NullConditionSpec copy = *null;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* cf = std::get_if<CrossFieldConditionSpec>(&child.node)) {
            CrossFieldConditionSpec copy = *cf;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* bf = std::get_if<BitfieldConditionSpec>(&child.node)) {
            BitfieldConditionSpec copy = *bf;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* cidr = std::get_if<CidrConditionSpec>(&child.node)) {
            CidrConditionSpec copy = *cidr;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* regex = std::get_if<RegexConditionSpec>(&child.node)) {
            RegexConditionSpec copy = *regex;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* lookup = std::get_if<LookupConditionSpec>(&child.node)) {
            LookupConditionSpec copy = *lookup;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* win = std::get_if<WindowConditionSpec>(&child.node)) {
            WindowConditionSpec copy = *win;
            copy.op = invert_op(copy.op);
            return (*this)(copy);
        }
        if (const auto* arr = std::get_if<ArrayAnyConditionSpec>(&child.node)) {
            int input = (*this)(*arr);
            int reg = ctx.next_register++;
            ctx.seq.push_back(BitwiseNotOp{input, reg});
            return reg;
        }

        int input = compile(child);
        int reg = ctx.next_register++;
        ctx.seq.push_back(BitwiseNotOp{input, reg});
        return reg;
    }
};

uint64_t double_bits(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

int predicate_column(const KernelOp& op) {
    if (const auto* n = std::get_if<NumericPredicateOp>(&op)) return n->column_index;
    if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) return r->column_index;
    if (const auto* c = std::get_if<CategoricalPredicateOp>(&op)) return c->column_index;
    if (const auto* a = std::get_if<ArrayLenPredicateOp>(&op)) return a->column_index;
    if (const auto* n = std::get_if<NullPredicateOp>(&op)) return n->column_index;
    if (const auto* c = std::get_if<CrossFieldPredicateOp>(&op)) return c->left_column_index;
    if (const auto* b = std::get_if<BitfieldPredicateOp>(&op)) return b->column_index;
    if (const auto* s = std::get_if<StringPredicateOp>(&op)) return s->column_index;
    if (const auto* r = std::get_if<RegexPredicateOp>(&op)) return r->column_index;
    if (const auto* l = std::get_if<LookupPredicateOp>(&op)) return l->column_index;
    if (const auto* c = std::get_if<CidrPredicateOp>(&op)) return c->column_index;
    if (const auto* t = std::get_if<TemporalPredicateOp>(&op)) return t->column_index;
    if (const auto* g = std::get_if<GeoDistancePredicateOp>(&op)) return g->lat_column_index;
    if (const auto* a = std::get_if<ArithmeticPredicateOp>(&op)) {
        if (a->root_value >= 0 && a->root_value < static_cast<int>(a->value_nodes.size())) {
            const auto& root = a->value_nodes[a->root_value];
            if (root.column_index >= 0) return root.column_index;
        }
        return a->other_column_index;
    }
    if (const auto* w = std::get_if<WindowPredicateOp>(&op)) return w->window_column_index;
    return -1;
}

int predicate_cost(const KernelOp& op) {
    if (std::holds_alternative<CategoricalPredicateOp>(op)) return 1;
    if (std::holds_alternative<ArrayLenPredicateOp>(op)) return 1;
    if (std::holds_alternative<NullPredicateOp>(op)) return 1;
    if (std::holds_alternative<BitfieldPredicateOp>(op)) return 1;
    if (std::holds_alternative<NumericPredicateOp>(op)) return 2;
    if (std::holds_alternative<CrossFieldPredicateOp>(op)) return 2;
    if (std::holds_alternative<TemporalPredicateOp>(op)) return 2;
    if (std::holds_alternative<CidrPredicateOp>(op)) return 3;
    if (std::holds_alternative<LookupPredicateOp>(op)) return 3;
    if (std::holds_alternative<NumericRangePredicateOp>(op)) return 3;
    if (std::holds_alternative<StringPredicateOp>(op)) return 5;
    if (std::holds_alternative<RegexPredicateOp>(op)) return 6;
    if (std::holds_alternative<ArithmeticPredicateOp>(op)) return 5;
    if (std::holds_alternative<WindowPredicateOp>(op)) return 4;
    if (std::holds_alternative<GeoDistancePredicateOp>(op)) return 6;
    return 8;
}

std::string textual_key(const Textual& values) {
    std::ostringstream os;
    if (std::holds_alternative<std::string>(values)) {
        os << "s:" << std::get<std::string>(values);
    } else {
        os << "v:";
        for (const auto& value : std::get<std::vector<std::string>>(values)) os << value << '\x1f';
    }
    return os.str();
}

std::string predicate_key(const KernelOp& op) {
    std::ostringstream os;
    if (const auto* n = std::get_if<NumericPredicateOp>(&op)) {
        os << "n|" << n->column_index << '|' << static_cast<int>(n->column_type)
           << '|' << static_cast<int>(n->op_type) << '|' << double_bits(n->threshold);
    } else if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) {
        os << "r|" << r->column_index << '|' << static_cast<int>(r->column_type)
           << '|' << static_cast<int>(r->op_type) << '|' << double_bits(r->lower)
           << '|' << double_bits(r->upper);
    } else if (const auto* c = std::get_if<CategoricalPredicateOp>(&op)) {
        os << "c|" << c->column_index << '|' << static_cast<int>(c->op_type)
           << '|' << textual_key(c->raw_values);
    } else if (const auto* a = std::get_if<ArrayLenPredicateOp>(&op)) {
        os << "al|" << a->column_index << '|' << static_cast<int>(a->op_type)
           << '|' << a->length;
    } else if (const auto* n = std::get_if<NullPredicateOp>(&op)) {
        os << "z|" << n->column_index << '|' << static_cast<int>(n->op_type);
    } else if (const auto* c = std::get_if<CrossFieldPredicateOp>(&op)) {
        os << "x|" << c->left_column_index << '|' << c->right_column_index
           << '|' << static_cast<int>(c->left_type) << '|' << static_cast<int>(c->right_type)
           << '|' << static_cast<int>(c->op_type);
    } else if (const auto* b = std::get_if<BitfieldPredicateOp>(&op)) {
        os << "b|" << b->column_index << '|' << static_cast<int>(b->column_type)
           << '|' << static_cast<int>(b->op_type) << '|' << b->mask;
    } else if (const auto* s = std::get_if<StringPredicateOp>(&op)) {
        os << "s|" << s->column_index << '|' << static_cast<int>(s->op_type)
           << '|' << s->pattern << '|' << s->length;
    } else if (const auto* r = std::get_if<RegexPredicateOp>(&op)) {
        os << "re|" << r->column_index << '|' << static_cast<int>(r->op_type)
           << '|' << r->pattern;
    } else if (const auto* l = std::get_if<LookupPredicateOp>(&op)) {
        uint64_t generation = l->lookup ? l->lookup->generation : 0;
        int type = l->lookup ? static_cast<int>(l->lookup->type) : -1;
        os << "lk|" << l->column_index << '|' << static_cast<int>(l->column_type)
           << '|' << static_cast<int>(l->op_type) << '|' << l->lookup_name
           << '|' << type << '|' << generation;
    } else if (const auto* c = std::get_if<CidrPredicateOp>(&op)) {
        os << "ip|" << c->column_index << '|' << static_cast<int>(c->column_type)
           << '|' << static_cast<int>(c->op_type) << '|' << c->network << '|' << c->mask;
    } else if (const auto* t = std::get_if<TemporalPredicateOp>(&op)) {
        os << "t|" << t->column_index << '|' << static_cast<int>(t->op_type)
           << '|' << t->value << '|' << double_bits(t->lower) << '|' << double_bits(t->upper);
        for (int v : t->values) os << '|' << v;
    } else if (const auto* g = std::get_if<GeoDistancePredicateOp>(&op)) {
        os << "g|" << g->lat_column_index << '|' << g->lon_column_index << '|'
           << g->other_lat_column_index << '|' << g->other_lon_column_index
           << '|' << static_cast<int>(g->op_type) << '|' << double_bits(g->threshold_km);
    } else if (const auto* a = std::get_if<ArithmeticPredicateOp>(&op)) {
        os << "a|" << static_cast<int>(a->op_type) << '|' << double_bits(a->threshold)
           << '|' << a->other_column_index << '|' << static_cast<int>(a->other_column_type);
        for (const auto& node : a->value_nodes) {
            os << "|n:" << static_cast<int>(node.kind) << ',' << node.column_index << ','
               << static_cast<int>(node.column_type) << ',' << double_bits(node.literal)
               << ',' << node.left << ',' << node.right;
        }
    } else if (const auto* w = std::get_if<WindowPredicateOp>(&op)) {
        os << "w|" << w->window_column_index << '|' << static_cast<int>(w->column_type)
           << '|' << static_cast<int>(w->op_type) << '|' << double_bits(w->threshold);
    }
    return os.str();
}

KernelOp predicate_with_output(KernelOp op, int output_register) {
    if (auto* n = std::get_if<NumericPredicateOp>(&op)) n->output_register = output_register;
    else if (auto* r = std::get_if<NumericRangePredicateOp>(&op)) r->output_register = output_register;
    else if (auto* c = std::get_if<CategoricalPredicateOp>(&op)) c->output_register = output_register;
    else if (auto* a = std::get_if<ArrayLenPredicateOp>(&op)) a->output_register = output_register;
    else if (auto* n = std::get_if<NullPredicateOp>(&op)) n->output_register = output_register;
    else if (auto* c = std::get_if<CrossFieldPredicateOp>(&op)) c->output_register = output_register;
    else if (auto* b = std::get_if<BitfieldPredicateOp>(&op)) b->output_register = output_register;
    else if (auto* s = std::get_if<StringPredicateOp>(&op)) s->output_register = output_register;
    else if (auto* r = std::get_if<RegexPredicateOp>(&op)) r->output_register = output_register;
    else if (auto* l = std::get_if<LookupPredicateOp>(&op)) l->output_register = output_register;
    else if (auto* c = std::get_if<CidrPredicateOp>(&op)) c->output_register = output_register;
    else if (auto* t = std::get_if<TemporalPredicateOp>(&op)) t->output_register = output_register;
    else if (auto* g = std::get_if<GeoDistancePredicateOp>(&op)) g->output_register = output_register;
    else if (auto* a = std::get_if<ArithmeticPredicateOp>(&op)) a->output_register = output_register;
    else if (auto* w = std::get_if<WindowPredicateOp>(&op)) w->output_register = output_register;
    return op;
}

bool is_predicate(const KernelOp& op) {
    return std::holds_alternative<NumericPredicateOp>(op) ||
           std::holds_alternative<NumericRangePredicateOp>(op) ||
           std::holds_alternative<CategoricalPredicateOp>(op) ||
           std::holds_alternative<ArrayLenPredicateOp>(op) ||
           std::holds_alternative<NullPredicateOp>(op) ||
           std::holds_alternative<CrossFieldPredicateOp>(op) ||
           std::holds_alternative<BitfieldPredicateOp>(op) ||
           std::holds_alternative<StringPredicateOp>(op) ||
           std::holds_alternative<RegexPredicateOp>(op) ||
           std::holds_alternative<LookupPredicateOp>(op) ||
           std::holds_alternative<CidrPredicateOp>(op) ||
           std::holds_alternative<TemporalPredicateOp>(op) ||
           std::holds_alternative<GeoDistancePredicateOp>(op) ||
           std::holds_alternative<ArithmeticPredicateOp>(op) ||
           std::holds_alternative<WindowPredicateOp>(op);
}

int output_register_of(const KernelOp& op) {
    if (const auto* n = std::get_if<NumericPredicateOp>(&op)) return n->output_register;
    if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) return r->output_register;
    if (const auto* c = std::get_if<CategoricalPredicateOp>(&op)) return c->output_register;
    if (const auto* a = std::get_if<ArrayLenPredicateOp>(&op)) return a->output_register;
    if (const auto* z = std::get_if<NullPredicateOp>(&op)) return z->output_register;
    if (const auto* c = std::get_if<CrossFieldPredicateOp>(&op)) return c->output_register;
    if (const auto* b = std::get_if<BitfieldPredicateOp>(&op)) return b->output_register;
    if (const auto* s = std::get_if<StringPredicateOp>(&op)) return s->output_register;
    if (const auto* r = std::get_if<RegexPredicateOp>(&op)) return r->output_register;
    if (const auto* l = std::get_if<LookupPredicateOp>(&op)) return l->output_register;
    if (const auto* c = std::get_if<CidrPredicateOp>(&op)) return c->output_register;
    if (const auto* t = std::get_if<TemporalPredicateOp>(&op)) return t->output_register;
    if (const auto* g = std::get_if<GeoDistancePredicateOp>(&op)) return g->output_register;
    if (const auto* a = std::get_if<ArithmeticPredicateOp>(&op)) return a->output_register;
    if (const auto* w = std::get_if<WindowPredicateOp>(&op)) return w->output_register;
    if (const auto* a = std::get_if<BitwiseAndOp>(&op)) return a->output_register;
    if (const auto* o = std::get_if<BitwiseOrOp>(&op)) return o->output_register;
    if (const auto* n = std::get_if<BitwiseNotOp>(&op)) return n->output_register;
    return -1;
}

int expr_cost(const RuleEvalPlan& plan, int node_idx, const GlobalPredicatePlan& global) {
    const RuleExprNode& node = plan.nodes[node_idx];
    if (node.kind == RuleExprKind::PREDICATE) {
        return predicate_cost(global.predicates[node.predicate_index]);
    }
    int cost = (node.kind == RuleExprKind::NOT) ? 6 : 5;
    for (int child : node.children) cost = std::min(cost, expr_cost(plan, child, global));
    return cost;
}

void build_global_plan(CompiledRuleSet& compiled) {
    absl::flat_hash_map<std::string, int> predicate_ids;
    compiled.global_plan.enabled = true;
    compiled.global_plan.rule_plans.clear();
    compiled.global_plan.rule_plans.reserve(compiled.rules.size());

    for (const EvalKernelSequence& rule : compiled.rules) {
        RuleEvalPlan plan;
        std::vector<int> register_node(rule.register_count, -1);

        for (const KernelOp& op : rule.op) {
            int output = output_register_of(op);
            if (is_predicate(op)) {
                std::string key = predicate_key(op);
                auto [it, inserted] = predicate_ids.emplace(key, static_cast<int>(compiled.global_plan.predicates.size()));
                int predicate_id = it->second;
                if (inserted) {
                    compiled.global_plan.predicates.push_back(predicate_with_output(op, predicate_id));
                }

                RuleExprNode node;
                node.kind = RuleExprKind::PREDICATE;
                node.predicate_index = predicate_id;
                register_node[output] = static_cast<int>(plan.nodes.size());
                plan.nodes.push_back(std::move(node));
            } else if (const auto* and_op = std::get_if<BitwiseAndOp>(&op)) {
                RuleExprNode node;
                node.kind = RuleExprKind::AND;
                for (int reg : and_op->input_registers) node.children.push_back(register_node[reg]);
                node.original_children = node.children;
                std::sort(node.children.begin(), node.children.end(), [&](int a, int b) {
                    return expr_cost(plan, a, compiled.global_plan) < expr_cost(plan, b, compiled.global_plan);
                });
                register_node[output] = static_cast<int>(plan.nodes.size());
                plan.nodes.push_back(std::move(node));
            } else if (const auto* or_op = std::get_if<BitwiseOrOp>(&op)) {
                RuleExprNode node;
                node.kind = RuleExprKind::OR;
                for (int reg : or_op->input_registers) node.children.push_back(register_node[reg]);
                register_node[output] = static_cast<int>(plan.nodes.size());
                plan.nodes.push_back(std::move(node));
            } else if (const auto* not_op = std::get_if<BitwiseNotOp>(&op)) {
                RuleExprNode node;
                node.kind = RuleExprKind::NOT;
                node.children.push_back(register_node[not_op->input_register]);
                register_node[output] = static_cast<int>(plan.nodes.size());
                plan.nodes.push_back(std::move(node));
            }
        }

        plan.root_node = register_node[rule.final_register];
        compiled.global_plan.rule_plans.push_back(std::move(plan));
    }

    int n = static_cast<int>(compiled.global_plan.predicates.size());
    compiled.global_plan.predicate_eval_order.resize(n);
    for (int i = 0; i < n; ++i) compiled.global_plan.predicate_eval_order[i] = i;
    std::sort(compiled.global_plan.predicate_eval_order.begin(),
              compiled.global_plan.predicate_eval_order.end(),
              [&](int a, int b) {
                  int ca = predicate_column(compiled.global_plan.predicates[a]);
                  int cb = predicate_column(compiled.global_plan.predicates[b]);
                  if (ca != cb) return ca < cb;
                  int pa = predicate_cost(compiled.global_plan.predicates[a]);
                  int pb = predicate_cost(compiled.global_plan.predicates[b]);
                  if (pa != pb) return pa < pb;
                  return a < b;
              });
}

bool validate_field(const BlazeRulesSchema& schema, const std::string& field, const std::string& rule_id,
                    BlazeRulesError& error) {
    if (schema.has_field(field)) return true;
    error = {BlazeRulesError::UNKNOWN_FIELD_NAME, "unknown field: " + field, "schema", rule_id};
    return false;
}

bool validate_numeric_field(const BlazeRulesSchema& schema, const std::string& field,
                            const std::string& rule_id, BlazeRulesError& error) {
    if (!validate_field(schema, field, rule_id, error)) return false;
    if (is_numeric(schema.type_of(schema.index_of(field)))) return true;
    error = {BlazeRulesError::TYPE_MISMATCH, "numeric operator used on non-numeric field: " + field,
             "schema", rule_id};
    return false;
}

bool validate_text_field(const BlazeRulesSchema& schema, const std::string& field,
                         const std::string& rule_id, BlazeRulesError& error) {
    if (!validate_field(schema, field, rule_id, error)) return false;
    ColumnType t = schema.type_of(schema.index_of(field));
    if (is_text(t)) return true;
    error = {BlazeRulesError::TYPE_MISMATCH, "text operator used on non-text field: " + field,
             "schema", rule_id};
    return false;
}

bool validate_string_field(const BlazeRulesSchema& schema, const std::string& field,
                           const std::string& rule_id, BlazeRulesError& error) {
    if (!validate_field(schema, field, rule_id, error)) return false;
    if (schema.type_of(schema.index_of(field)) == ColumnType::STRING) return true;
    error = {BlazeRulesError::TYPE_MISMATCH, "string pattern operator requires STRING field: " + field,
             "schema", rule_id};
    return false;
}

bool validate_expr(const BlazeRulesSchema& schema, const ArithmeticExprSpec& expr,
                   const std::string& rule_id, BlazeRulesError& error) {
    if (expr.kind == ArithmeticExprKind::FIELD) {
        return validate_numeric_field(schema, expr.field, rule_id, error);
    }
    for (const auto& child : expr.children) {
        if (!validate_expr(schema, child, rule_id, error)) return false;
    }
    return true;
}

bool validate_condition(const BlazeRulesSchema& schema, const ConditionSpec& c,
                        const std::string& rule_id, const LookupRegistry& lookups, BlazeRulesError& error) {
    if (const auto* n = std::get_if<NumericConditionSpec>(&c.node)) {
        return validate_numeric_field(schema, n->field, rule_id, error);
    }
    if (const auto* r = std::get_if<NumericRangeConditionSpec>(&c.node)) {
        return validate_numeric_field(schema, r->field, rule_id, error);
    }
    if (const auto* cat = std::get_if<CategoricalConditionSpec>(&c.node)) {
        if (!validate_field(schema, cat->field, rule_id, error)) return false;
        ColumnType t = schema.type_of(schema.index_of(cat->field));
        if (t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY) return true;
        error = {BlazeRulesError::TYPE_MISMATCH, "categorical operator requires CATEGORICAL/ENTITY_KEY field: " + cat->field,
                 "schema", rule_id};
        return false;
    }
    if (const auto* arr = std::get_if<ArrayLenConditionSpec>(&c.node)) {
        if (!validate_field(schema, arr->field, rule_id, error)) return false;
        ColumnType t = schema.type_of(schema.index_of(arr->field));
        if (t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY || t == ColumnType::STRING) return true;
        error = {BlazeRulesError::TYPE_MISMATCH, "array_len operator requires CATEGORICAL/ENTITY_KEY/STRING field: " + arr->field,
                 "schema", rule_id};
        return false;
    }
    if (const auto* z = std::get_if<NullConditionSpec>(&c.node)) {
        if (z->op == OpType::IS_EMPTY || z->op == OpType::IS_NOT_EMPTY) {
            return validate_text_field(schema, z->field, rule_id, error);
        }
        return validate_field(schema, z->field, rule_id, error);
    }
    if (const auto* cf = std::get_if<CrossFieldConditionSpec>(&c.node)) {
        return validate_field(schema, cf->field, rule_id, error) &&
               validate_field(schema, cf->other_field, rule_id, error);
    }
    if (const auto* bf = std::get_if<BitfieldConditionSpec>(&c.node)) {
        return validate_numeric_field(schema, bf->field, rule_id, error);
    }
    if (const auto* s = std::get_if<StringConditionSpec>(&c.node)) {
        return validate_string_field(schema, s->field, rule_id, error);
    }
    if (const auto* r = std::get_if<RegexConditionSpec>(&c.node)) {
        if (!validate_string_field(schema, r->field, rule_id, error)) return false;
        RE2 regex(r->pattern);
        if (regex.ok()) return true;
        error = {BlazeRulesError::UNKNOWN_OP, "invalid regex pattern: " + regex.error(),
                 "rule", rule_id};
        return false;
    }
    if (const auto* l = std::get_if<LookupConditionSpec>(&c.node)) {
        if (!validate_field(schema, l->field, rule_id, error)) return false;
        auto it = lookups.find(l->lookup_name);
        if (it == lookups.end()) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD, "unknown lookup: " + l->lookup_name,
                     "lookup", rule_id};
            return false;
        }
        ColumnType t = schema.type_of(schema.index_of(l->field));
        LookupSetType lookup_type = it->second->type;
        bool ok = false;
        if (lookup_type == LookupSetType::STRING_SET) {
            ok = t == ColumnType::STRING || t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY;
        } else if (lookup_type == LookupSetType::INT_SET) {
            ok = t == ColumnType::INT32 || t == ColumnType::INT64 || t == ColumnType::TIMESTAMP_MS;
        } else {
            ok = t == ColumnType::STRING || t == ColumnType::INT32 || t == ColumnType::INT64;
        }
        if (ok) return true;
        error = {BlazeRulesError::TYPE_MISMATCH, "lookup type incompatible with field: " + l->field,
                 "lookup", rule_id};
        return false;
    }
    if (const auto* cidr = std::get_if<CidrConditionSpec>(&c.node)) {
        if (!validate_field(schema, cidr->field, rule_id, error)) return false;
        ColumnType t = schema.type_of(schema.index_of(cidr->field));
        if (t == ColumnType::STRING || t == ColumnType::INT32 || t == ColumnType::INT64) return true;
        error = {BlazeRulesError::TYPE_MISMATCH, "CIDR operator requires STRING/INT32/INT64 field: " + cidr->field,
                 "schema", rule_id};
        return false;
    }
    if (const auto* t = std::get_if<TemporalConditionSpec>(&c.node)) {
        return validate_numeric_field(schema, t->field, rule_id, error);
    }
    if (const auto* g = std::get_if<GeoDistanceConditionSpec>(&c.node)) {
        return validate_numeric_field(schema, g->lat_field, rule_id, error) &&
               validate_numeric_field(schema, g->lon_field, rule_id, error) &&
               validate_numeric_field(schema, g->other_lat_field, rule_id, error) &&
               validate_numeric_field(schema, g->other_lon_field, rule_id, error);
    }
    if (const auto* a = std::get_if<ArithmeticConditionSpec>(&c.node)) {
        if (!validate_expr(schema, a->expr, rule_id, error)) return false;
        if (!a->other_field.empty()) return validate_numeric_field(schema, a->other_field, rule_id, error);
        return true;
    }
    if (const auto* w = std::get_if<WindowConditionSpec>(&c.node)) {
        if (!validate_field(schema, w->field, rule_id, error)) return false;
        if (w->windowfn == WindowFn::AVG && w->sum_field.empty()) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD, "avg window requires sum_field",
                     "schema", rule_id};
            return false;
        }
        if ((w->windowfn == WindowFn::MIN || w->windowfn == WindowFn::MAX) && w->sum_field.empty()) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD,
                     "min/max window requires a value field (sum_field/value_field)",
                     "schema", rule_id};
            return false;
        }
        if (w->windowfn == WindowFn::RATIO &&
            (w->sum_field.empty() || w->denominator_field.empty())) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD,
                     "ratio window requires numerator_field/sum_field and denominator_field",
                     "schema", rule_id};
            return false;
        }
        if (!w->sum_field.empty() &&
            !validate_numeric_field(schema, w->sum_field, rule_id, error)) {
            return false;
        }
        if (!w->denominator_field.empty() &&
            !validate_numeric_field(schema, w->denominator_field, rule_id, error)) {
            return false;
        }
        return true;
    }
    if (const auto* ms = std::get_if<ModelScoreConditionSpec>(&c.node)) {
#ifndef BLAZERULES_ENABLE_ONNX
        error = {BlazeRulesError::UNKNOWN_OP,
                 "model_score requires building with BLAZERULES_ENABLE_ONNX (ONNX Runtime)",
                 "rule", rule_id};
        return false;
#endif
        if (ms->model_name.empty()) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD, "model_score requires a model name",
                     "rule", rule_id};
            return false;
        }
        if (ms->features.empty()) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD,
                     "model_score requires at least one feature field", "rule", rule_id};
            return false;
        }
        for (const auto& f : ms->features) {
            if (!validate_numeric_field(schema, f, rule_id, error)) return false;
        }
        return true;
    }
    if (const auto* vd = std::get_if<VectorDistanceConditionSpec>(&c.node)) {
        if (vd->dims.empty()) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD,
                     "vector_distance requires at least one embedding dim field", "rule", rule_id};
            return false;
        }
        if (vd->reference.size() != vd->dims.size()) {
            error = {BlazeRulesError::TYPE_MISMATCH,
                     "vector_distance reference length must equal the number of dims", "rule", rule_id};
            return false;
        }
        for (const auto& f : vd->dims) {
            if (!validate_numeric_field(schema, f, rule_id, error)) return false;
        }
        return true;
    }
    if (const auto* arr = std::get_if<ArrayAnyConditionSpec>(&c.node)) {
        if (!validate_numeric_field(schema, arr->synthetic_field, rule_id, error)) return false;
        if (!arr->where) {
            error = {BlazeRulesError::MISSING_REQUIRED_FIELD,
                     "array_any requires where condition", "rule", rule_id};
            return false;
        }
        return true;
    }
    if (const auto* a = std::get_if<AndConditionSpec>(&c.node)) {
        for (const auto& child : a->child_condition) {
            if (!validate_condition(schema, child, rule_id, lookups, error)) return false;
        }
        return true;
    }
    if (const auto* o = std::get_if<OrConditionSpec>(&c.node)) {
        for (const auto& child : o->child_condition) {
            if (!validate_condition(schema, child, rule_id, lookups, error)) return false;
        }
        return true;
    }
    if (const auto* n = std::get_if<NotConditionSpec>(&c.node)) {
        for (const auto& child : n->child_condition) {
            if (!validate_condition(schema, child, rule_id, lookups, error)) return false;
        }
        return true;
    }
    return true;
}

} // namespace

CompileResult compile_rule_file(const RuleFileSpec& spec, const BlazeRulesSchema& schema) {
    CompiledRuleSet compiled;
    compiled.name = spec.name;
    compiled.version = spec.version;
    compiled.loaded_at = now_iso8601();
    compiled.default_decision = spec.default_decision.empty() ? "APPROVE" : spec.default_decision;
    auto register_decision = [&](const std::string& label) -> int {
        auto it = compiled.decision_label_to_code.find(label);
        if (it != compiled.decision_label_to_code.end()) return it->second;
        int code = static_cast<int>(compiled.decision_labels.size());
        compiled.decision_labels.push_back(label);
        compiled.decision_label_to_code.emplace(label, code);
        compiled.decision_ranks.push_back(0);
        return code;
    };
    register_decision(compiled.default_decision);
    for (size_t i = 0; i < spec.decision_precedence.size(); ++i) {
        int code = register_decision(spec.decision_precedence[i]);
        if (code >= static_cast<int>(compiled.decision_ranks.size())) {
            compiled.decision_ranks.resize(static_cast<size_t>(code + 1), 0);
        }
        compiled.decision_ranks[static_cast<size_t>(code)] = static_cast<int>(i);
    }
    for (const RuleSpec& rule_spec : spec.rules) {
        int code = register_decision(rule_spec.action_label);
        if (code >= static_cast<int>(compiled.decision_ranks.size())) {
            compiled.decision_ranks.resize(static_cast<size_t>(code + 1), 0);
        }
        if (compiled.decision_ranks[static_cast<size_t>(code)] == 0 &&
            rule_spec.action_label != compiled.default_decision) {
            compiled.decision_ranks[static_cast<size_t>(code)] =
                static_cast<int>(spec.decision_precedence.size());
        }
    }
    auto lookups = load_lookup_registry(spec);
    if (lookups.is_error()) {
        return CompileResult{false, {}, lookups.error(), {lookups.error()}};
    }
    compiled.lookups = std::move(lookups.value());

    std::vector<BlazeRulesError> diagnostics;
    std::unordered_set<std::string> seen_rule_ids;
    seen_rule_ids.reserve(spec.rules.size());
    for (const RuleSpec& rule_spec : spec.rules) {
        if (!seen_rule_ids.insert(rule_spec.id).second) {
            BlazeRulesError error{BlazeRulesError::DUPLICATE_RULE_ID,
                           "duplicate rule id: " + rule_spec.id,
                           "rule", rule_spec.id};
            error.domain = BlazeRulesError::Domain::RULE_VALIDATION;
            diagnostics.push_back(std::move(error));
        }
        BlazeRulesError validation_error;
        if (!validate_condition(schema, rule_spec.root_condition, rule_spec.id,
                                compiled.lookups, validation_error)) {
            validation_error.domain = validation_error.source == "lookup"
                ? BlazeRulesError::Domain::LOOKUP
                : BlazeRulesError::Domain::RULE_VALIDATION;
            diagnostics.push_back(std::move(validation_error));
        }
    }
    if (!diagnostics.empty()) {
        return CompileResult{false, {}, diagnostics.front(), std::move(diagnostics)};
    }

    compiled.rules.reserve(spec.rules.size());
    for (const RuleSpec& rule_spec : spec.rules) {
        EvalKernelSequence ks;
        ks.rule_id = rule_spec.id;
        ks.rule_name = rule_spec.name;
        ks.rule_version = rule_spec.version;
        ks.action = rule_spec.action;
        ks.action_label = rule_spec.action_label;
        auto decision_it = compiled.decision_label_to_code.find(ks.action_label);
        ks.action_code = decision_it == compiled.decision_label_to_code.end() ? 0 : decision_it->second;
        ks.action_rank = ks.action_code >= 0 && ks.action_code < static_cast<int>(compiled.decision_ranks.size())
            ? compiled.decision_ranks[static_cast<size_t>(ks.action_code)]
            : 0;
        ks.severity = rule_spec.severity;
        ks.priority = rule_spec.priority;
        ks.weight = rule_spec.weight;
        ks.score_cap = rule_spec.score_cap;
        ks.reason_code = rule_spec.reason_code.empty() ? rule_spec.id : rule_spec.reason_code;
        ks.shadow = rule_spec.shadow;
        ks.enabled = rule_spec.enabled;

        int next_register = 0;
        CompileCtx ctx{schema, ks.op, next_register, compiled.window_channels,
                       compiled.model_channels, compiled.vector_channels,
                       compiled.array_any_channels,
                       compiled.derived_plan, compiled.lookups};
        CompileVisitor visitor{ctx};
        ks.final_register = visitor.compile(rule_spec.root_condition);
        ks.register_count = next_register;

        compiled.rule_id_to_index.emplace(ks.rule_id, static_cast<int>(compiled.rules.size()));
        compiled.rules.push_back(std::move(ks));
    }

    build_global_plan(compiled);

    return CompileResult{true, std::move(compiled), BlazeRulesError{}};
}
