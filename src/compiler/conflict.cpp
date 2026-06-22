#include "blazerules/conflict.h"

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();

struct Interval {
    double lo = -INF, hi = INF;
    bool lo_incl = true, hi_incl = true;
    bool empty() const {
        if (lo > hi) return true;
        if (lo == hi && !(lo_incl && hi_incl)) return true;
        return false;
    }
};

Interval from_op(OpType op, double v) {
    Interval iv;
    switch (op) {
        case OpType::GT:  iv.lo = v; iv.lo_incl = false; break;
        case OpType::GTE: iv.lo = v; iv.lo_incl = true;  break;
        case OpType::LT:  iv.hi = v; iv.hi_incl = false; break;
        case OpType::LTE: iv.hi = v; iv.hi_incl = true;  break;
        case OpType::EQ:  iv.lo = iv.hi = v; break;
        default: break;
    }
    return iv;
}

Interval intersect(const Interval& a, const Interval& b) {
    Interval r;
    if (a.lo > b.lo) { r.lo = a.lo; r.lo_incl = a.lo_incl; }
    else if (b.lo > a.lo) { r.lo = b.lo; r.lo_incl = b.lo_incl; }
    else { r.lo = a.lo; r.lo_incl = a.lo_incl && b.lo_incl; }
    if (a.hi < b.hi) { r.hi = a.hi; r.hi_incl = a.hi_incl; }
    else if (b.hi < a.hi) { r.hi = b.hi; r.hi_incl = b.hi_incl; }
    else { r.hi = a.hi; r.hi_incl = a.hi_incl && b.hi_incl; }
    return r;
}

bool contained(const Interval& a, const Interval& b) {
    bool lo_ok = (a.lo > b.lo) || (a.lo == b.lo && (b.lo_incl || !a.lo_incl));
    bool hi_ok = (a.hi < b.hi) || (a.hi == b.hi && (b.hi_incl || !a.hi_incl));
    return lo_ok && hi_ok;
}

struct RuleConstraints {
    std::map<std::string, Interval> numeric;
    std::map<std::string, std::set<std::string>> in_sets;
    bool dead = false;
};

void add_numeric(RuleConstraints& rc, const std::string& field, const Interval& iv) {
    auto it = rc.numeric.find(field);
    if (it == rc.numeric.end()) rc.numeric[field] = iv;
    else {
        it->second = intersect(it->second, iv);
        if (it->second.empty()) rc.dead = true;
    }
}

void collect(const ConditionSpec& cond, RuleConstraints& rc) {
    if (const auto* a = std::get_if<AndConditionSpec>(&cond.node)) {
        for (const auto& c : a->child_condition) collect(c, rc);
    } else if (std::get_if<OrConditionSpec>(&cond.node)) {
    } else if (std::get_if<NotConditionSpec>(&cond.node)) {
    } else if (const auto* n = std::get_if<NumericConditionSpec>(&cond.node)) {
        add_numeric(rc, n->field, from_op(n->op, n->threshold));
    } else if (const auto* r = std::get_if<NumericRangeConditionSpec>(&cond.node)) {
        Interval iv;
        iv.lo = r->lower; iv.hi = r->upper;
        iv.lo_incl = iv.hi_incl = (r->op == OpType::BETWEEN_INCLUDING);
        add_numeric(rc, r->field, iv);
    } else if (const auto* c = std::get_if<CategoricalConditionSpec>(&cond.node)) {
        if (c->op == OpType::IN || c->op == OpType::EQ) {
            std::set<std::string>& s = rc.in_sets[c->field];
            if (std::holds_alternative<std::string>(c->values)) {
                s.insert(std::get<std::string>(c->values));
            } else {
                for (const auto& v : std::get<std::vector<std::string>>(c->values)) s.insert(v);
            }
        }
    }
}

bool disjoint(const std::set<std::string>& a, const std::set<std::string>& b) {
    for (const auto& x : a) if (b.count(x)) return false;
    return true;
}
bool subset(const std::set<std::string>& a, const std::set<std::string>& b) {
    for (const auto& x : a) if (!b.count(x)) return false;
    return true;
}

} // namespace

ConflictReport analyze_conflicts(const RuleFileSpec& spec) {
    ConflictReport rep;
    rep.total_rules_analyzed = static_cast<int>(spec.rules.size());

    std::vector<RuleConstraints> cons(spec.rules.size());
    for (size_t i = 0; i < spec.rules.size(); ++i) {
        collect(spec.rules[i].root_condition, cons[i]);
        if (cons[i].dead)
            rep.dead_rules.push_back({spec.rules[i].id, "all conditions impossible on one field"});
    }

    for (size_t i = 0; i < spec.rules.size(); ++i) {
        for (size_t j = i + 1; j < spec.rules.size(); ++j) {
            const auto& ci = cons[i];
            const auto& cj = cons[j];
            const std::string& ida = spec.rules[i].id;
            const std::string& idb = spec.rules[j].id;

            bool any_shared = false, all_i_in_j = true, all_j_in_i = true;

            for (const auto& [field, iv_i] : ci.numeric) {
                auto it = cj.numeric.find(field);
                if (it == cj.numeric.end()) continue;
                any_shared = true;
                const Interval& iv_j = it->second;
                if (intersect(iv_i, iv_j).empty()) {
                    rep.conflicts.push_back({ida, idb, field,
                        "no value satisfies both rules on '" + field + "'"});
                    all_i_in_j = all_j_in_i = false;
                } else {
                    if (!contained(iv_i, iv_j)) all_i_in_j = false;
                    if (!contained(iv_j, iv_i)) all_j_in_i = false;
                }
            }

            for (const auto& [field, set_i] : ci.in_sets) {
                auto it = cj.in_sets.find(field);
                if (it == cj.in_sets.end()) continue;
                any_shared = true;
                const auto& set_j = it->second;
                if (disjoint(set_i, set_j)) {
                    rep.conflicts.push_back({ida, idb, field,
                        "disjoint value sets on '" + field + "'"});
                    all_i_in_j = all_j_in_i = false;
                } else {
                    if (!subset(set_i, set_j)) all_i_in_j = false;
                    if (!subset(set_j, set_i)) all_j_in_i = false;
                }
            }

            if (any_shared && all_i_in_j && !all_j_in_i)
                rep.subsumptions.push_back({ida, idb, "", ida + " is stricter than " + idb});
            else if (any_shared && all_j_in_i && !all_i_in_j)
                rep.subsumptions.push_back({idb, ida, "", idb + " is stricter than " + ida});
        }
    }
    return rep;
}

std::string ConflictReport::to_string() const {
    std::ostringstream os;
    os << "ConflictReport(rules_analyzed=" << total_rules_analyzed
       << ", conflicts=" << conflicts.size()
       << ", subsumptions=" << subsumptions.size()
       << ", dead_rules=" << dead_rules.size() << ")";
    for (const auto& c : conflicts)
        os << "\n  CONFLICT " << c.rule_a_id << " <> " << c.rule_b_id << ": " << c.explanation;
    for (const auto& s : subsumptions)
        os << "\n  SUBSUMES " << s.subsumer_rule_id << " >= " << s.subsumed_rule_id << ": " << s.explanation;
    for (const auto& d : dead_rules)
        os << "\n  DEAD " << d.rule_id << ": " << d.reason;
    return os.str();
}
