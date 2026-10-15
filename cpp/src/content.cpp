#include "content.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

#include "json.hpp"

namespace moba {

using json::Value;
using Obj = std::vector<std::pair<std::string, Value>>;

// ------------------------------------------------------------------------------ equality

bool AbilityDef::operator==(const AbilityDef& o) const {
    return name == o.name && desc == o.desc && cast == o.cast && dtype == o.dtype && fx == o.fx && max_level == o.max_level &&
           req_level == o.req_level && range == o.range && radius == o.radius && mana == o.mana && cd == o.cd && dmg == o.dmg &&
           dur == o.dur && pct == o.pct && pct2 == o.pct2 && speed == o.speed && delay == o.delay && aoe_on_land == o.aoe_on_land &&
           icon == o.icon && color == o.color;
}
bool HeroDef::operator==(const HeroDef& o) const {
    return name == o.name && title == o.title && primary == o.primary && str_ == o.str_ && agi == o.agi && int_ == o.int_ &&
           gain_str == o.gain_str && gain_agi == o.gain_agi && gain_int == o.gain_int && dmg_min == o.dmg_min && dmg_max == o.dmg_max &&
           range == o.range && bat == o.bat && ms == o.ms && proj_speed == o.proj_speed && radius == o.radius && build == o.build &&
           skill_order == o.skill_order && glyph == o.glyph && color == o.color;
}
bool ItemDef::operator==(const ItemDef& o) const {
    return name == o.name && cost == o.cost && str_ == o.str_ && agi == o.agi && int_ == o.int_ && dmg == o.dmg && armor == o.armor &&
           hp == o.hp && mana == o.mana && aspd == o.aspd && ms == o.ms && hpreg == o.hpreg && mpreg == o.mpreg && active == o.active &&
           cd == o.cd && stack == o.stack && color == o.color;
}
bool CreepDef::operator==(const CreepDef& o) const {
    return name == o.name && kind == o.kind && hp == o.hp && dmg_min == o.dmg_min && dmg_max == o.dmg_max && armor == o.armor &&
           range == o.range && bat == o.bat && ms == o.ms && proj_speed == o.proj_speed && radius == o.radius && vision == o.vision &&
           gold == o.gold && xp == o.xp && bldg_mult == o.bldg_mult;
}
bool TowerDef::operator==(const TowerDef& o) const {
    return hp == o.hp && dmg_min == o.dmg_min && dmg_max == o.dmg_max && armor == o.armor && range == o.range && bat == o.bat &&
           gold == o.gold && lane_frac == o.lane_frac;
}
bool AncientDef::operator==(const AncientDef& o) const {
    return hp == o.hp && armor == o.armor && radius == o.radius && vision == o.vision;
}
bool Content::operator==(const Content& o) const {
    return heroes == o.heroes && abilities == o.abilities && items == o.items && melee == o.melee && ranged == o.ranged &&
           siege == o.siege && neutrals == o.neutrals && camp_comp == o.camp_comp && towers == o.towers && ancient == o.ancient &&
           xp_table == o.xp_table && rules == o.rules && mods == o.mods;
}

// ------------------------------------------------------------------------------ rules table

namespace {

/// One row per rule, in declaration order (the export lists them in this order).
struct RuleSpec {
    const char* name;
    float Rules::*f;     // set for f32 rules
    uint32_t Rules::*u;  // set for u32 rules
    double lo, hi;
};

#define RF(n, lo, hi) RuleSpec{#n, &Rules::n, nullptr, (double)(lo), (double)(hi)}
#define RU(n, lo, hi) RuleSpec{#n, nullptr, &Rules::n, (double)(lo), (double)(hi)}
const RuleSpec RULE_TABLE[] = {
    RF(start_gold, 0, 1000000),     RF(passive_gold, 0, 10000),     RF(wave_interval, 1, 600),   RF(first_wave_at, 0, 600),
    RF(camp_respawn, 1, 3600),      RF(xp_radius, 0, 5000),         RF(shop_radius, 0, 5000),    RF(magic_resist, 0, 0.95),
    RF(blink_range, 0, 5000),       RF(salve_heal, 0, 100000),      RF(salve_time, 0.1, 300),    RF(mana_potion_restore, 0, 100000),
    RF(mana_potion_time, 0.1, 300), RF(hero_vision, 0, 5000),       RF(hp_base, 1, 100000),      RF(hp_per_str, 0, 10000),
    RF(mana_base, 0, 100000),       RF(mana_per_int, 0, 10000),     RF(armor_base, -50, 100),    RF(armor_per_agi, -5, 5),
    RF(hp_regen_base, 0, 1000),     RF(hp_regen_per_str, 0, 100),   RF(mana_regen_base, 0, 1000), RF(mana_regen_per_int, 0, 100),
    RF(respawn_base, 0, 600),       RF(respawn_per_level, 0, 100),  RF(respawn_max, 0, 600),     RF(hero_bounty_base, 0, 100000),
    RF(hero_bounty_per_level, 0, 10000), RF(hero_xp_base, 0, 100000), RF(hero_xp_per_level, 0, 10000),
    RU(wave_melee, 0, 20),          RU(wave_ranged, 0, 20),         RU(wave_siege, 0, 10),       RU(siege_every, 0, 1000),
};
#undef RF
#undef RU

// ------------------------------------------------------------------------------ enum names

template <typename T>
struct Named {
    const char* name;
    T value;
};
const Named<Cast> CASTS[] = {{"none", Cast::None},         {"target", Cast::Target},       {"line", Cast::Line},  {"point", Cast::PointAoE},
                             {"self_aoe", Cast::SelfAoE}, {"self_buff", Cast::SelfBuff}, {"blink", Cast::Blink}};
const Named<DmgType> DTYPES[] = {{"physical", DmgType::Physical}, {"magical", DmgType::Magical}, {"pure", DmgType::Pure}};
const Named<uint8_t> FXS[] = {{"stun", FX_STUN}, {"slow", FX_SLOW}, {"root", FX_ROOT}, {"haste", FX_HASTE}, {"aspd", FX_ASPD}};
const Named<uint8_t> ACTIVES[] = {{"none", ACT_NONE}, {"salve", ACT_SALVE}, {"mana", ACT_MANA}, {"blink", ACT_BLINK}};
const Named<uint8_t> PRIMARIES[] = {{"str", 0}, {"agi", 1}, {"int", 2}};

template <typename T, size_t N>
const char* name_of(const Named<T> (&t)[N], T v, const char* dflt) {
    for (auto& e : t)
        if (e.value == v) return e.name;
    return dflt;
}
template <typename T, size_t N>
std::string names_of(const Named<T> (&t)[N]) {
    std::string s;
    for (size_t i = 0; i < N; ++i) {
        if (i) s += ", ";
        s += t[i].name;
    }
    return s;
}
std::vector<std::string> fx_names(uint8_t fx) {
    std::vector<std::string> v;
    for (auto& e : FXS)
        if (fx & e.value) v.push_back(e.name);
    return v;
}

}  // namespace

Rules Rules::builtin() { return make_builtin_content().rules; }

bool Rules::operator==(const Rules& o) const {
    for (auto& r : RULE_TABLE) {
        if (r.f && this->*(r.f) != o.*(r.f)) return false;
        if (r.u && this->*(r.u) != o.*(r.u)) return false;
    }
    return true;
}

std::shared_ptr<const Content> builtin() {
    static std::shared_ptr<const Content> b = std::make_shared<const Content>(make_builtin_content());
    return b;
}

// ------------------------------------------------------------------------------ export

std::string Content::to_mod_json() const {
    auto s = [](const std::string& t) { return Value::str(t); };
    auto f = [](float v) { return Value::flt(v); };
    auto lvl = [&](const std::array<float, 4>& a) {
        if (a[1] == a[0] && a[2] == a[0] && a[3] == a[0]) return f(a[0]);
        return Value::array({f(a[0]), f(a[1]), f(a[2]), f(a[3])});
    };
    auto creep = [&](const CreepDef& c) {
        return Value::object({{"name", s(c.name)},
                              {"hp", f(c.hp)},
                              {"dmg_min", f(c.dmg_min)},
                              {"dmg_max", f(c.dmg_max)},
                              {"armor", f(c.armor)},
                              {"range", f(c.range)},
                              {"bat", f(c.bat)},
                              {"ms", f(c.ms)},
                              {"proj_speed", f(c.proj_speed)},
                              {"radius", f(c.radius)},
                              {"vision", f(c.vision)},
                              {"gold", f(c.gold)},
                              {"xp", f(c.xp)},
                              {"bldg_mult", f(c.bldg_mult)}});
    };
    Obj o;
    o.emplace_back("format", Value::num(1.0));
    o.emplace_back("name", s(mods.empty() ? std::string("Built-in content") : mods.back().name));
    o.emplace_back("comment", s("Every value of the built-in game. Applying this file changes nothing; copy the parts you want to change into your own mod."));
    Obj ro;
    for (auto& r : RULE_TABLE) ro.emplace_back(r.name, r.f ? f(rules.*(r.f)) : Value::num((double)(rules.*(r.u))));
    o.emplace_back("rules", Value::object(std::move(ro)));
    std::vector<Value> xp;
    for (float x : xp_table) xp.push_back(f(x));
    o.emplace_back("xp_table", Value::array(std::move(xp)));

    std::vector<Value> its;
    for (size_t i = 1; i < items.size(); ++i) {
        const ItemDef& it = items[i];
        Obj v{{"name", s(it.name)}, {"cost", f(it.cost)}};
        const std::pair<const char*, float> stats[] = {{"str", it.str_}, {"agi", it.agi},     {"int", it.int_},   {"dmg", it.dmg},
                                                       {"armor", it.armor}, {"hp", it.hp},    {"mana", it.mana},  {"aspd", it.aspd},
                                                       {"ms", it.ms},       {"hpreg", it.hpreg}, {"mpreg", it.mpreg}};
        for (auto& kv : stats)
            if (kv.second != 0.0f) v.emplace_back(kv.first, f(kv.second));
        if (it.active != ACT_NONE) v.emplace_back("active", s(name_of(ACTIVES, it.active, "none")));
        if (it.cd != 0.0f) v.emplace_back("cd", f(it.cd));
        if (it.stack) v.emplace_back("stack", Value::boolean(true));
        if (!it.color.empty()) v.emplace_back("color", s(it.color));
        its.push_back(Value::object(std::move(v)));
    }
    o.emplace_back("items", Value::array(std::move(its)));
    o.emplace_back("creeps", Value::object({{"melee", creep(melee)}, {"ranged", creep(ranged)}, {"siege", creep(siege)}}));
    std::vector<Value> ns;
    for (auto& n : neutrals) ns.push_back(creep(n));
    o.emplace_back("neutrals", Value::array(std::move(ns)));
    std::vector<Value> camps;
    for (auto& c : camp_comp) {
        std::vector<Value> names;
        for (uint8_t n : c) names.push_back(s(neutrals[n].name));
        camps.push_back(Value::array(std::move(names)));
    }
    o.emplace_back("camps", Value::array(std::move(camps)));
    std::vector<Value> tw;
    for (auto& t : towers) {
        tw.push_back(Value::object({{"hp", f(t.hp)},
                                    {"dmg_min", f(t.dmg_min)},
                                    {"dmg_max", f(t.dmg_max)},
                                    {"armor", f(t.armor)},
                                    {"range", f(t.range)},
                                    {"bat", f(t.bat)},
                                    {"gold", f(t.gold)},
                                    {"lane_frac", f(t.lane_frac)}}));
    }
    o.emplace_back("towers", Value::array(std::move(tw)));
    o.emplace_back("ancient", Value::object({{"hp", f(ancient.hp)}, {"armor", f(ancient.armor)}, {"radius", f(ancient.radius)}, {"vision", f(ancient.vision)}}));

    std::vector<Value> hs;
    for (size_t hi = 0; hi < heroes.size(); ++hi) {
        const HeroDef& h = heroes[hi];
        std::vector<Value> abs;
        for (size_t sl = 0; sl < 4; ++sl) {
            const AbilityDef& a = abilities[hi * 4 + sl];
            std::vector<Value> fx;
            for (auto& n : fx_names(a.fx)) fx.push_back(s(n));
            std::vector<Value> req;
            for (uint8_t r : a.req_level) req.push_back(Value::num(r));
            Obj v{{"name", s(a.name)},
                  {"desc", s(a.desc)},
                  {"cast", s(name_of(CASTS, a.cast, "none"))},
                  {"dtype", s(name_of(DTYPES, a.dtype, "magical"))},
                  {"fx", Value::array(std::move(fx))},
                  {"max_level", Value::num(a.max_level)},
                  {"req_level", Value::array(std::move(req))},
                  {"range", lvl(a.range)},
                  {"radius", lvl(a.radius)},
                  {"mana", lvl(a.mana)},
                  {"cd", lvl(a.cd)},
                  {"dmg", lvl(a.dmg)},
                  {"dur", lvl(a.dur)},
                  {"pct", lvl(a.pct)},
                  {"pct2", lvl(a.pct2)},
                  {"speed", f(a.speed)},
                  {"delay", f(a.delay)},
                  {"aoe_on_land", Value::boolean(a.aoe_on_land)}};
            if (!a.icon.empty()) v.emplace_back("icon", s(a.icon));
            if (!a.color.empty()) v.emplace_back("color", s(a.color));
            abs.push_back(Value::object(std::move(v)));
        }
        std::vector<Value> build, order;
        for (uint8_t i : h.build) build.push_back(s(items[i].name));
        for (uint8_t x : h.skill_order) order.push_back(Value::num(x));
        static const char* const PRIM[] = {"str", "agi", "int"};
        Obj v{{"name", s(h.name)},
              {"title", s(h.title)},
              {"primary", s(PRIM[std::min<uint8_t>(h.primary, 2)])},
              {"str", f(h.str_)},
              {"agi", f(h.agi)},
              {"int", f(h.int_)},
              {"gain_str", f(h.gain_str)},
              {"gain_agi", f(h.gain_agi)},
              {"gain_int", f(h.gain_int)},
              {"dmg_min", f(h.dmg_min)},
              {"dmg_max", f(h.dmg_max)},
              {"range", f(h.range)},
              {"bat", f(h.bat)},
              {"ms", f(h.ms)},
              {"proj_speed", f(h.proj_speed)},
              {"radius", f(h.radius)},
              {"build", Value::array(std::move(build))},
              {"skill_order", Value::array(std::move(order))}};
        if (!h.glyph.empty()) v.emplace_back("glyph", s(h.glyph));
        if (!h.color.empty()) v.emplace_back("color", s(h.color));
        v.emplace_back("abilities", Value::array(std::move(abs)));
        hs.push_back(Value::object(std::move(v)));
    }
    o.emplace_back("heroes", Value::array(std::move(hs)));
    return json::to_pretty(Value::object(std::move(o)));
}

// ------------------------------------------------------------------------------ validation helpers

namespace {

constexpr size_t MAX_ERRORS = 20;

struct Cx {
    std::vector<std::string> errs;
    void err(const std::string& path, const std::string& msg) {
        if (errs.size() < MAX_ERRORS) errs.push_back(path + ": " + msg);
        else if (errs.size() == MAX_ERRORS) errs.push_back("... more errors omitted");
    }
};

std::string idx_path(const std::string& p, size_t i) { return p + "[" + std::to_string(i) + "]"; }
std::string fmt_bound(double v) { return json::fmt_f64(v); }

bool get_num(Cx& cx, const std::string& path, const Value& v, double lo, double hi, double& out) {
    if (v.type != Value::Num) {
        cx.err(path, std::string("expected a number, found ") + v.type_name());
        return false;
    }
    if (v.n < lo || v.n > hi) {
        cx.err(path, "must be between " + fmt_bound(lo) + " and " + fmt_bound(hi));
        return false;
    }
    out = v.n;
    return true;
}

bool get_int(Cx& cx, const std::string& path, const Value& v, int64_t lo, int64_t hi, int64_t& out) {
    double n;
    if (!get_num(cx, path, v, (double)lo, (double)hi, n)) return false;
    if (n != std::trunc(n)) {
        cx.err(path, "must be a whole number");
        return false;
    }
    out = (int64_t)n;
    return true;
}

bool get_str(Cx& cx, const std::string& path, const Value& v, size_t max, bool allow_empty, std::string& out) {
    if (v.type != Value::Str) {
        cx.err(path, std::string("expected a string, found ") + v.type_name());
        return false;
    }
    if (v.s.empty() && !allow_empty) {
        cx.err(path, "must not be empty");
        return false;
    }
    if (v.s.size() > max) {
        cx.err(path, "is too long (at most " + std::to_string(max) + " bytes)");
        return false;
    }
    out = v.s;
    return true;
}

bool get_color(Cx& cx, const std::string& path, const Value& v, std::string& out) {
    std::string s;
    if (!get_str(cx, path, v, 7, true, s)) return false;
    bool ok = s.empty();
    if (s.size() == 7 && s[0] == '#') {
        ok = std::all_of(s.begin() + 1, s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
    }
    if (!ok) {
        cx.err(path, "expected a colour like \"#ff8800\"");
        return false;
    }
    out = s;
    return true;
}

bool get_bool(Cx& cx, const std::string& path, const Value& v, bool& out) {
    if (v.type != Value::Bool) {
        cx.err(path, std::string("expected true or false, found ") + v.type_name());
        return false;
    }
    out = v.b;
    return true;
}

const std::vector<Value>* get_arr(Cx& cx, const std::string& path, const Value& v) {
    if (v.type != Value::Arr) {
        cx.err(path, std::string("expected an array, found ") + v.type_name());
        return nullptr;
    }
    return &v.arr;
}

const Obj* get_obj(Cx& cx, const std::string& path, const Value& v) {
    if (v.type != Value::Obj) {
        cx.err(path, std::string("expected an object, found ") + v.type_name());
        return nullptr;
    }
    return &v.obj;
}

template <typename T, size_t N>
bool get_enum(Cx& cx, const std::string& path, const Value& v, const Named<T> (&table)[N], const char* what, T& out) {
    std::string s;
    if (!get_str(cx, path, v, 40, true, s)) return false;
    for (auto& e : table) {
        if (s == e.name) {
            out = e.value;
            return true;
        }
    }
    cx.err(path, std::string("unknown ") + what + " \"" + s + "\" (expected one of: " + names_of(table) + ")");
    return false;
}

/// A per-level value: one number (all levels) or 1-4 numbers (the last one repeats).
void get_levels(Cx& cx, const std::string& path, const Value& v, double lo, double hi, std::array<float, 4>& out) {
    if (v.type == Value::Num) {
        double n;
        if (get_num(cx, path, v, lo, hi, n)) out.fill((float)n);
        return;
    }
    if (v.type == Value::Arr && v.arr.size() >= 1 && v.arr.size() <= 4) {
        std::array<float, 4> tmp = out;
        bool ok = true;
        for (size_t i = 0; i < v.arr.size(); ++i) {
            double n;
            if (get_num(cx, idx_path(path, i), v.arr[i], lo, hi, n)) tmp[i] = (float)n;
            else ok = false;
        }
        if (ok) {
            for (size_t i = v.arr.size(); i < 4; ++i) tmp[i] = tmp[v.arr.size() - 1];
            out = tmp;
        }
        return;
    }
    cx.err(path, "expected a number or an array of 1 to 4 numbers");
}

void get_req_levels(Cx& cx, const std::string& path, const Value& v, std::array<uint8_t, 4>& out) {
    bool is_arr = v.type == Value::Arr;
    std::vector<const Value*> vals;
    if (is_arr && v.arr.size() >= 1 && v.arr.size() <= 4) {
        for (auto& x : v.arr) vals.push_back(&x);
    } else if (v.type == Value::Num) {
        vals.push_back(&v);
    } else {
        cx.err(path, "expected a number or an array of 1 to 4 numbers");
        return;
    }
    std::array<uint8_t, 4> tmp = out;
    bool ok = true;
    for (size_t i = 0; i < vals.size(); ++i) {
        std::string p = is_arr ? idx_path(path, i) : path;
        int64_t n;
        if (get_int(cx, p, *vals[i], 1, 99, n)) tmp[i] = (uint8_t)n;
        else ok = false;
    }
    if (ok) {
        for (size_t i = vals.size(); i < 4; ++i) tmp[i] = tmp[vals.size() - 1];
        out = tmp;
    }
}

void numf(Cx& cx, const std::string& p, const Value& v, float& slot, double lo, double hi) {
    double n;
    if (get_num(cx, p, v, lo, hi, n)) slot = (float)n;
}

void require(Cx& cx, const std::string& path, const std::vector<std::string>& seen, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) cx.err(path, std::string("missing required key \"") + k + "\"");
}

const Value* find_key(const Obj& obj, const char* key) {
    for (auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

/// Name of a list entry (`name` is required and must be a string).
bool entry_name(Cx& cx, const std::string& path, const Obj& obj, std::string& out) {
    const Value* v = find_key(obj, "name");
    if (!v) {
        cx.err(path, "missing required key \"name\"");
        return false;
    }
    return get_str(cx, path + ".name", *v, 40, false, out);
}

// ------------------------------------------------------------------------------ patching

void patch_rules(Cx& cx, const std::string& path, const Obj& obj, Rules& r) {
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        std::string p = path + "." + k;
        const RuleSpec* spec = nullptr;
        for (auto& s : RULE_TABLE)
            if (k == s.name) spec = &s;
        if (!spec) {
            cx.err(p, "unknown key");
            continue;
        }
        double n;
        if (!get_num(cx, p, v, spec->lo, spec->hi, n)) continue;
        if (spec->u) {
            if (n != std::trunc(n)) cx.err(p, "must be a whole number");
            else r.*(spec->u) = (uint32_t)n;
        } else {
            r.*(spec->f) = (float)n;
        }
    }
}

void patch_ability(Cx& cx, const std::string& path, const Obj& obj, AbilityDef& a, bool is_new) {
    std::vector<std::string> seen;
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        seen.push_back(k);
        std::string p = path + "." + k;
        std::string s;
        if (k == "name") {
            if (get_str(cx, p, v, 40, false, s)) a.name = s;
        } else if (k == "desc") {
            if (get_str(cx, p, v, 300, true, s)) a.desc = s;
        } else if (k == "cast") {
            get_enum(cx, p, v, CASTS, "cast type", a.cast);
        } else if (k == "dtype") {
            get_enum(cx, p, v, DTYPES, "damage type", a.dtype);
        } else if (k == "fx") {
            if (auto arr = get_arr(cx, p, v)) {
                uint8_t bits = 0;
                bool ok = true;
                for (size_t i = 0; i < arr->size(); ++i) {
                    uint8_t b;
                    if (get_enum(cx, idx_path(p, i), (*arr)[i], FXS, "effect", b)) bits |= b;
                    else ok = false;
                }
                if (ok) a.fx = bits;
            }
        } else if (k == "max_level") {
            int64_t n;
            if (get_int(cx, p, v, 1, 4, n)) a.max_level = (uint8_t)n;
        } else if (k == "req_level") {
            get_req_levels(cx, p, v, a.req_level);
        } else if (k == "range") {
            get_levels(cx, p, v, 0.0, 5000.0, a.range);
        } else if (k == "radius") {
            get_levels(cx, p, v, 0.0, 2000.0, a.radius);
        } else if (k == "mana") {
            get_levels(cx, p, v, 0.0, 100000.0, a.mana);
        } else if (k == "cd") {
            get_levels(cx, p, v, 0.0, 10000.0, a.cd);
        } else if (k == "dmg") {
            get_levels(cx, p, v, 0.0, 1000000.0, a.dmg);
        } else if (k == "dur") {
            get_levels(cx, p, v, 0.0, 300.0, a.dur);
        } else if (k == "pct") {
            get_levels(cx, p, v, 0.0, 1000.0, a.pct);
        } else if (k == "pct2") {
            get_levels(cx, p, v, 0.0, 1000.0, a.pct2);
        } else if (k == "speed") {
            numf(cx, p, v, a.speed, 0, 10000);
        } else if (k == "delay") {
            numf(cx, p, v, a.delay, 0, 30);
        } else if (k == "aoe_on_land") {
            get_bool(cx, p, v, a.aoe_on_land);
        } else if (k == "icon") {
            if (get_str(cx, p, v, 24, true, s)) a.icon = s;
        } else if (k == "color") {
            get_color(cx, p, v, a.color);
        } else {
            cx.err(p, "unknown key");
        }
    }
    if (is_new) require(cx, path, seen, {"name", "cast"});
}

void patch_hero(Cx& cx, const std::string& path, const Obj& obj, const std::vector<ItemDef>& c_items, HeroDef& h,
                std::array<AbilityDef, 4>& abilities, bool is_new) {
    std::vector<std::string> seen;
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        seen.push_back(k);
        std::string p = path + "." + k;
        std::string s;
        if (k == "name") {
            // already used to find the entry
        } else if (k == "title") {
            if (get_str(cx, p, v, 80, true, s)) h.title = s;
        } else if (k == "primary") {
            get_enum(cx, p, v, PRIMARIES, "primary attribute", h.primary);
        } else if (k == "str") numf(cx, p, v, h.str_, 0, 1000);
        else if (k == "agi") numf(cx, p, v, h.agi, 0, 1000);
        else if (k == "int") numf(cx, p, v, h.int_, 0, 1000);
        else if (k == "gain_str") numf(cx, p, v, h.gain_str, 0, 100);
        else if (k == "gain_agi") numf(cx, p, v, h.gain_agi, 0, 100);
        else if (k == "gain_int") numf(cx, p, v, h.gain_int, 0, 100);
        else if (k == "dmg_min") numf(cx, p, v, h.dmg_min, 0, 100000);
        else if (k == "dmg_max") numf(cx, p, v, h.dmg_max, 0, 100000);
        else if (k == "range") numf(cx, p, v, h.range, 0, 5000);
        else if (k == "bat") numf(cx, p, v, h.bat, 0.1, 20);
        else if (k == "ms") numf(cx, p, v, h.ms, 0, 2000);
        else if (k == "proj_speed") numf(cx, p, v, h.proj_speed, 0, 10000);
        else if (k == "radius") numf(cx, p, v, h.radius, 1, 500);
        else if (k == "glyph") {
            if (get_str(cx, p, v, 24, true, s)) h.glyph = s;
        } else if (k == "color") {
            get_color(cx, p, v, h.color);
        } else if (k == "build") {
            if (auto arr = get_arr(cx, p, v)) {
                if (arr->size() > 24) {
                    cx.err(p, "has too many items (at most 24)");
                } else {
                    std::vector<uint8_t> ids;
                    bool ok = true;
                    for (size_t i = 0; i < arr->size(); ++i) {
                        std::string ip = idx_path(p, i), n;
                        if (get_str(cx, ip, (*arr)[i], 40, false, n)) {
                            auto it = std::find_if(c_items.begin(), c_items.end(), [&](const ItemDef& d) { return d.name == n; });
                            size_t idx = (size_t)(it - c_items.begin());
                            if (it != c_items.end() && idx > 0) {
                                ids.push_back((uint8_t)idx);
                            } else {
                                cx.err(ip, "unknown item \"" + n + "\"");
                                ok = false;
                            }
                        } else {
                            ok = false;
                        }
                    }
                    if (ok) h.build = ids;
                }
            }
        } else if (k == "skill_order") {
            if (auto arr = get_arr(cx, p, v)) {
                if (arr->size() != 3) {
                    cx.err(p, "expected exactly 3 ability numbers (0, 1 or 2)");
                } else {
                    auto tmp = h.skill_order;
                    bool ok = true;
                    for (size_t i = 0; i < 3; ++i) {
                        int64_t n;
                        if (get_int(cx, idx_path(p, i), (*arr)[i], 0, 2, n)) tmp[i] = (uint8_t)n;
                        else ok = false;
                    }
                    if (ok) h.skill_order = tmp;
                }
            }
        } else if (k == "abilities") {
            if (auto arr = get_arr(cx, p, v)) {
                if (arr->size() > 4 || (is_new && arr->size() != 4)) {
                    cx.err(p, is_new ? "a new hero needs exactly 4 abilities" : "expected at most 4 abilities");
                } else {
                    for (size_t i = 0; i < arr->size(); ++i) {
                        const Value& av = (*arr)[i];
                        if (av.type == Value::Null && !is_new) continue;
                        std::string ap = idx_path(p, i);
                        if (auto ao = get_obj(cx, ap, av)) patch_ability(cx, ap, *ao, abilities[i], is_new);
                    }
                }
            }
        } else {
            cx.err(p, "unknown key");
        }
    }
    if (is_new) require(cx, path, seen, {"abilities"});
    if (h.dmg_min > h.dmg_max) cx.err(path, "dmg_min must not exceed dmg_max");
}

void patch_item(Cx& cx, const std::string& path, const Obj& obj, ItemDef& it, bool is_new) {
    std::vector<std::string> seen;
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        seen.push_back(k);
        std::string p = path + "." + k;
        if (k == "name") {
        } else if (k == "cost") numf(cx, p, v, it.cost, 0, 1000000);
        else if (k == "str") numf(cx, p, v, it.str_, -1000, 1000);
        else if (k == "agi") numf(cx, p, v, it.agi, -1000, 1000);
        else if (k == "int") numf(cx, p, v, it.int_, -1000, 1000);
        else if (k == "dmg") numf(cx, p, v, it.dmg, -10000, 10000);
        else if (k == "armor") numf(cx, p, v, it.armor, -100, 100);
        else if (k == "hp") numf(cx, p, v, it.hp, -100000, 100000);
        else if (k == "mana") numf(cx, p, v, it.mana, -100000, 100000);
        else if (k == "aspd") numf(cx, p, v, it.aspd, -1000, 1000);
        else if (k == "ms") numf(cx, p, v, it.ms, -1000, 1000);
        else if (k == "hpreg") numf(cx, p, v, it.hpreg, -1000, 1000);
        else if (k == "mpreg") numf(cx, p, v, it.mpreg, -1000, 1000);
        else if (k == "active") get_enum(cx, p, v, ACTIVES, "active effect", it.active);
        else if (k == "cd") numf(cx, p, v, it.cd, 0, 10000);
        else if (k == "stack") get_bool(cx, p, v, it.stack);
        else if (k == "color") get_color(cx, p, v, it.color);
        else cx.err(p, "unknown key");
    }
    if (is_new) require(cx, path, seen, {"cost"});
}

void patch_creep(Cx& cx, const std::string& path, const Obj& obj, CreepDef& c, bool is_new, bool name_ok) {
    std::vector<std::string> seen;
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        seen.push_back(k);
        std::string p = path + "." + k;
        if (k == "name") {
            std::string s;
            if (name_ok && get_str(cx, p, v, 40, false, s)) c.name = s;
        } else if (k == "hp") numf(cx, p, v, c.hp, 1, 10000000);
        else if (k == "dmg_min") numf(cx, p, v, c.dmg_min, 0, 100000);
        else if (k == "dmg_max") numf(cx, p, v, c.dmg_max, 0, 100000);
        else if (k == "armor") numf(cx, p, v, c.armor, -100, 100);
        else if (k == "range") numf(cx, p, v, c.range, 0, 5000);
        else if (k == "bat") numf(cx, p, v, c.bat, 0.1, 20);
        else if (k == "ms") numf(cx, p, v, c.ms, 0, 2000);
        else if (k == "proj_speed") numf(cx, p, v, c.proj_speed, 0, 10000);
        else if (k == "radius") numf(cx, p, v, c.radius, 1, 500);
        else if (k == "vision") numf(cx, p, v, c.vision, 0, 5000);
        else if (k == "gold") numf(cx, p, v, c.gold, 0, 1000000);
        else if (k == "xp") numf(cx, p, v, c.xp, 0, 1000000);
        else if (k == "bldg_mult") numf(cx, p, v, c.bldg_mult, 0, 100);
        else cx.err(p, "unknown key");
    }
    if (is_new) require(cx, path, seen, {"hp", "dmg_min", "dmg_max"});
    if (c.dmg_min > c.dmg_max) cx.err(path, "dmg_min must not exceed dmg_max");
}

void patch_tower(Cx& cx, const std::string& path, const Obj& obj, TowerDef& t) {
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        std::string p = path + "." + k;
        if (k == "hp") numf(cx, p, v, t.hp, 1, 10000000);
        else if (k == "dmg_min") numf(cx, p, v, t.dmg_min, 0, 100000);
        else if (k == "dmg_max") numf(cx, p, v, t.dmg_max, 0, 100000);
        else if (k == "armor") numf(cx, p, v, t.armor, -100, 100);
        else if (k == "range") numf(cx, p, v, t.range, 0, 5000);
        else if (k == "bat") numf(cx, p, v, t.bat, 0.1, 20);
        else if (k == "gold") numf(cx, p, v, t.gold, 0, 1000000);
        else if (k == "lane_frac") numf(cx, p, v, t.lane_frac, 0, 1);
        else cx.err(p, "unknown key");
    }
    if (t.dmg_min > t.dmg_max) cx.err(path, "dmg_min must not exceed dmg_max");
}

void patch_ancient(Cx& cx, const std::string& path, const Obj& obj, AncientDef& a) {
    for (auto& [k, v] : obj) {
        if (k == "comment") continue;
        std::string p = path + "." + k;
        if (k == "hp") numf(cx, p, v, a.hp, 1, 10000000);
        else if (k == "armor") numf(cx, p, v, a.armor, -100, 100);
        else if (k == "radius") numf(cx, p, v, a.radius, 1, 500);
        else if (k == "vision") numf(cx, p, v, a.vision, 0, 5000);
        else cx.err(p, "unknown key");
    }
}

const char* const TOP_KEYS[] = {"format", "name",     "version",  "author", "description", "replace", "rules", "xp_table",
                                "items",  "creeps",   "neutrals", "camps",  "towers",      "ancient", "heroes"};

/// For list sections: check each entry, its name and duplicates, then `patch(o, name, path)`.
void each_named(Cx& cx, const Value& v, const char* section, const std::function<void(const Obj&, const std::string&, const std::string&)>& patch) {
    std::string base = std::string("$.") + section;
    auto arr = get_arr(cx, base, v);
    if (!arr) return;
    std::vector<std::string> names;
    for (size_t i = 0; i < arr->size(); ++i) {
        std::string p = idx_path(base, i);
        auto o = get_obj(cx, p, (*arr)[i]);
        if (!o) continue;
        std::string name;
        if (!entry_name(cx, p, *o, name)) continue;
        auto dup = std::find(names.begin(), names.end(), name);
        if (dup != names.end()) {
            cx.err(p, "duplicate name \"" + name + "\" (also at " + idx_path(base, (size_t)(dup - names.begin())) + ")");
            continue;
        }
        names.push_back(name);
        patch(*o, name, p);
    }
}

}  // namespace

bool apply_mod(const Content& base, const std::string& bytes, Content& out, std::vector<std::string>& errors) {
    errors.clear();
    if (bytes.size() > MAX_MOD_BYTES) {
        errors.push_back("mod file is larger than 1 MiB");
        return false;
    }
    if (!json::valid_utf8((const unsigned char*)bytes.data(), bytes.size())) {
        errors.push_back("mod file is not valid UTF-8 text");
        return false;
    }
    Value root;
    std::string perr;
    if (!json::parse(bytes, root, perr)) {
        errors.push_back("not valid JSON: " + perr);
        return false;
    }
    if (root.type != Value::Obj) {
        errors.push_back(std::string("$: expected an object, found ") + root.type_name());
        return false;
    }
    const Obj& top = root.obj;
    Cx cx;
    Content c = base;

    for (auto& kv : top) {
        if (kv.first == "comment") continue;
        bool known = std::any_of(std::begin(TOP_KEYS), std::end(TOP_KEYS), [&](const char* k) { return kv.first == k; });
        if (!known) cx.err("$." + kv.first, "unknown key");
    }
    if (const Value* v = find_key(top, "format")) {
        double n;
        if (get_num(cx, "$.format", *v, 0.0, 1000000.0, n) && n != 1.0) cx.err("$.format", "unsupported mod format (this engine reads format 1)");
    }
    auto meta = [&](const char* key, size_t max) {
        std::string s;
        if (const Value* v = find_key(top, key)) get_str(cx, std::string("$.") + key, *v, max, true, s);
        return s;
    };
    ModInfo info;
    info.name = meta("name", 60);
    info.version = meta("version", 20);
    info.author = meta("author", 60);
    info.description = meta("description", 300);

    // ---- replace
    bool rep_heroes = false, rep_items = false, rep_neutrals = false;
    if (const Value* v = find_key(top, "replace")) {
        if (auto arr = get_arr(cx, "$.replace", *v)) {
            for (size_t i = 0; i < arr->size(); ++i) {
                std::string p = idx_path("$.replace", i), s;
                if (!get_str(cx, p, (*arr)[i], 40, true, s)) continue;
                if (s == "heroes") rep_heroes = true;
                else if (s == "items") rep_items = true;
                else if (s == "neutrals") rep_neutrals = true;
                else cx.err(p, "cannot replace \"" + s + "\" (expected one of: heroes, items, neutrals)");
            }
        }
    }
    if (rep_items && !rep_heroes)
        cx.err("$.replace", "replacing \"items\" also requires replacing \"heroes\" (their bot builds refer to the old items)");
    if (rep_neutrals && !find_key(top, "camps")) cx.err("$.replace", "replacing \"neutrals\" also requires a \"camps\" list");
    if (rep_heroes) {
        c.heroes.clear();
        c.abilities.clear();
    }
    if (rep_items) c.items.resize(1);
    if (rep_neutrals) {
        c.neutrals.clear();
        c.camp_comp.clear();
    }

    // ---- rules, xp table
    if (const Value* v = find_key(top, "rules"))
        if (auto o = get_obj(cx, "$.rules", *v)) patch_rules(cx, "$.rules", *o, c.rules);
    if (const Value* v = find_key(top, "xp_table")) {
        if (auto arr = get_arr(cx, "$.xp_table", *v)) {
            if (arr->size() < 2 || arr->size() > 60) {
                cx.err("$.xp_table", "must have between 2 and 60 entries");
            } else {
                std::vector<float> t;
                bool ok = true;
                for (size_t i = 0; i < arr->size(); ++i) {
                    double n;
                    if (get_num(cx, idx_path("$.xp_table", i), (*arr)[i], 0.0, 10000000.0, n)) t.push_back((float)n);
                    else ok = false;
                }
                if (ok) {
                    bool decreasing = false;
                    for (size_t i = 1; i < t.size(); ++i) decreasing = decreasing || t[i] < t[i - 1];
                    if (t[0] != 0.0f) cx.err("$.xp_table[0]", "level 1 must need 0 XP");
                    else if (decreasing) cx.err("$.xp_table", "values must not decrease");
                    else c.xp_table = t;
                }
            }
        }
    }

    // ---- items
    if (const Value* v = find_key(top, "items")) {
        each_named(cx, *v, "items", [&](const Obj& o, const std::string& name, const std::string& p) {
            size_t idx = 0;
            for (size_t k = 0; k < c.items.size() && !idx; ++k)
                if (c.items[k].name == name && !c.items[k].name.empty()) idx = k;
            if (idx) {
                ItemDef it = c.items[idx];
                patch_item(cx, p, o, it, false);
                c.items[idx] = it;
            } else {
                ItemDef it = c.items[0];
                it.name = name;
                patch_item(cx, p, o, it, true);
                c.items.push_back(it);
            }
        });
    }

    // ---- neutrals, creeps, camps
    if (const Value* v = find_key(top, "neutrals")) {
        each_named(cx, *v, "neutrals", [&](const Obj& o, const std::string& name, const std::string& p) {
            auto it = std::find_if(c.neutrals.begin(), c.neutrals.end(), [&](const CreepDef& d) { return d.name == name; });
            if (it != c.neutrals.end()) {
                CreepDef n = *it;
                patch_creep(cx, p, o, n, false, false);
                *it = n;
            } else {
                CreepDef n;
                n.name = name;
                n.kind = Kind::Neutral;
                n.hp = 0;
                n.dmg_min = 0;
                n.dmg_max = 0;
                n.armor = 0;
                n.range = 70;
                n.bat = 1;
                n.ms = 100;
                n.proj_speed = 0;
                n.radius = 12;
                n.vision = 260;
                n.gold = 0;
                n.xp = 0;
                n.bldg_mult = 1;
                patch_creep(cx, p, o, n, true, false);
                c.neutrals.push_back(n);
            }
        });
    }
    if (const Value* v = find_key(top, "creeps")) {
        if (auto o = get_obj(cx, "$.creeps", *v)) {
            for (auto& [k, cv] : *o) {
                if (k == "comment") continue;
                std::string p = "$.creeps." + k;
                CreepDef* slot = k == "melee" ? &c.melee : k == "ranged" ? &c.ranged : k == "siege" ? &c.siege : nullptr;
                if (!slot) {
                    cx.err(p, "unknown key (expected melee, ranged or siege)");
                    continue;
                }
                if (auto co = get_obj(cx, p, cv)) {
                    CreepDef tmp = *slot;
                    patch_creep(cx, p, *co, tmp, false, true);
                    *slot = tmp;
                }
            }
        }
    }
    if (const Value* v = find_key(top, "camps")) {
        if (auto arr = get_arr(cx, "$.camps", *v)) {
            if (arr->size() != 3) {
                cx.err("$.camps", "expected exactly 3 camp tiers (arrays of neutral names)");
            } else {
                std::vector<std::vector<uint8_t>> comps;
                bool ok = true;
                for (size_t t = 0; t < arr->size(); ++t) {
                    std::string tp = idx_path("$.camps", t);
                    std::vector<uint8_t> comp;
                    auto names = get_arr(cx, tp, (*arr)[t]);
                    if (names && !names->empty() && names->size() <= 8) {
                        for (size_t i = 0; i < names->size(); ++i) {
                            std::string np = idx_path(tp, i), n;
                            if (get_str(cx, np, (*names)[i], 40, false, n)) {
                                auto it = std::find_if(c.neutrals.begin(), c.neutrals.end(), [&](const CreepDef& d) { return d.name == n; });
                                if (it != c.neutrals.end()) {
                                    comp.push_back((uint8_t)(it - c.neutrals.begin()));
                                } else {
                                    cx.err(np, "unknown neutral \"" + n + "\"");
                                    ok = false;
                                }
                            } else {
                                ok = false;
                            }
                        }
                    } else if (names) {
                        cx.err(tp, "a camp needs between 1 and 8 neutrals");
                        ok = false;
                    } else {
                        ok = false;
                    }
                    comps.push_back(comp);
                }
                if (ok) c.camp_comp = comps;
            }
        }
    }

    // ---- towers, ancient
    if (const Value* v = find_key(top, "towers")) {
        if (auto arr = get_arr(cx, "$.towers", *v)) {
            if (arr->size() > 3) {
                cx.err("$.towers", "expected at most 3 tiers");
            } else {
                for (size_t i = 0; i < arr->size(); ++i) {
                    if ((*arr)[i].type == Value::Null) continue;
                    std::string p = idx_path("$.towers", i);
                    if (auto o = get_obj(cx, p, (*arr)[i])) {
                        TowerDef t = c.towers[i];
                        patch_tower(cx, p, *o, t);
                        c.towers[i] = t;
                    }
                }
            }
        }
    }
    if (const Value* v = find_key(top, "ancient")) {
        if (auto o = get_obj(cx, "$.ancient", *v)) {
            AncientDef a = c.ancient;
            patch_ancient(cx, "$.ancient", *o, a);
            c.ancient = a;
        }
    }

    // ---- heroes (after items so `build` can name them)
    if (const Value* v = find_key(top, "heroes")) {
        each_named(cx, *v, "heroes", [&](const Obj& o, const std::string& name, const std::string& p) {
            std::vector<ItemDef> items = c.items;
            auto it = std::find_if(c.heroes.begin(), c.heroes.end(), [&](const HeroDef& d) { return d.name == name; });
            if (it != c.heroes.end()) {
                size_t idx = (size_t)(it - c.heroes.begin());
                HeroDef h = c.heroes[idx];
                std::array<AbilityDef, 4> ab{c.abilities[idx * 4], c.abilities[idx * 4 + 1], c.abilities[idx * 4 + 2], c.abilities[idx * 4 + 3]};
                patch_hero(cx, p, o, items, h, ab, false);
                c.heroes[idx] = h;
                for (size_t s = 0; s < 4; ++s) c.abilities[idx * 4 + s] = ab[s];
            } else {
                HeroDef h;
                h.name = name;
                h.title = "";
                h.primary = 0;
                h.str_ = 20;
                h.agi = 20;
                h.int_ = 20;
                h.gain_str = 2;
                h.gain_agi = 2;
                h.gain_int = 2;
                h.dmg_min = 30;
                h.dmg_max = 36;
                h.range = 100;
                h.bat = 1.7f;
                h.ms = 300;
                h.proj_speed = 0;
                h.radius = 16;
                h.skill_order = {{0, 1, 2}};
                std::array<AbilityDef, 4> ab{};
                patch_hero(cx, p, o, items, h, ab, true);
                c.heroes.push_back(h);
                for (auto& a : ab) c.abilities.push_back(a);
            }
        });
    }

    // ---- whole-content checks (only meaningful when everything above was fine)
    if (cx.errs.empty()) {
        if (c.heroes.empty()) cx.err("$.heroes", "the game needs at least one hero");
        if (c.heroes.size() > MAX_HEROES) cx.err("$.heroes", "too many heroes (at most " + std::to_string(MAX_HEROES) + ")");
        if (c.items.size() > MAX_ITEMS) cx.err("$.items", "too many items (at most " + std::to_string(MAX_ITEMS - 1) + ")");
        if (c.neutrals.empty() || c.neutrals.size() > MAX_NEUTRALS)
            cx.err("$.neutrals", "the game needs between 1 and " + std::to_string(MAX_NEUTRALS) + " neutral types");
        if (c.camp_comp.size() != 3) cx.err("$.camps", "expected exactly 3 camp tiers (arrays of neutral names)");
    }

    if (!cx.errs.empty()) {
        errors = cx.errs;
        return false;
    }
    if (info.name.empty()) info.name = "Unnamed mod";
    c.mods.push_back(info);
    out = std::move(c);
    return true;
}

}  // namespace moba
