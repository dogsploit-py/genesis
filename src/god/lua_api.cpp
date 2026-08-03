#include "god/lua_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "core/config.h"
#include "god/god.h"
#include "sim/agent.h"
#include "sim/time.h"
#include "sim/world.h"

namespace gen {

namespace {

// The context for the currently-running chunk. A file-scope pointer is safe
// here because run() is only ever called from the sim thread, one chunk at a
// time, and it is cleared before returning.
LuaContext* g_ctx = nullptr;
LuaConsole* g_console = nullptr;

World&   W() { return *g_ctx->world; }
Agents&  A() { return *g_ctx->agents; }
GodMode& G() { return *g_ctx->god; }

int fail(lua_State* L, const char* msg) { return luaL_error(L, "%s", msg); }

bool ctxReady(lua_State* L) {
    if (!g_ctx || !g_ctx->world || !g_ctx->agents) { fail(L, "no simulation bound"); return false; }
    return true;
}

// Resolves an agent argument: either a uid (number) or a slot via agent.at().
int32_t slotArg(lua_State* L, int idx) {
    const lua_Integer uid = luaL_checkinteger(L, idx);
    const int32_t slot = A().slotOfUid(static_cast<uint64_t>(uid));
    if (slot < 0) luaL_error(L, "no living agent with uid %I", uid);
    return slot;
}

int traitIndexArg(lua_State* L, int idx) {
    if (lua_isnumber(L, idx)) {
        const int t = static_cast<int>(lua_tointeger(L, idx));
        if (t < 0 || t >= kTraitCount) luaL_error(L, "trait index %d out of range", t);
        return t;
    }
    const char* name = luaL_checkstring(L, idx);
    for (int t = 0; t < kTraitCount; ++t)
        if (std::strcmp(traitSpec(static_cast<Trait>(t)).name, name) == 0) return t;
    luaL_error(L, "unknown trait '%s'", name);
    return 0;
}

// -- world ------------------------------------------------------------------

int l_world_size(lua_State* L) {
    if (!ctxReady(L)) return 0;
    lua_pushinteger(L, W().width());
    lua_pushinteger(L, W().height());
    return 2;
}

int l_world_tick(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(g_ctx ? g_ctx->tick : 0));
    return 1;
}

int l_world_date(lua_State* L) {
    const DateTime d = tickToDate(g_ctx ? g_ctx->tick : 0);
    lua_pushstring(L, d.toString().c_str());
    return 1;
}

int l_world_tile(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int x = static_cast<int>(luaL_checkinteger(L, 1));
    const int y = static_cast<int>(luaL_checkinteger(L, 2));
    if (!W().inBounds(x, y)) return fail(L, "tile out of bounds");
    const size_t i = W().index(x, y);
    lua_newtable(L);
    auto setf = [&](const char* k, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, k);
    };
    auto sets = [&](const char* k, const char* v) {
        lua_pushstring(L, v);
        lua_setfield(L, -2, k);
    };
    setf("elevation", W().elevation[i]);
    setf("water", W().waterDepth[i]);
    setf("temperature", W().temperature[i]);
    setf("rainfall", W().rainfall[i]);
    setf("biomass", W().biomass[i]);
    setf("soilMoisture", W().soilMoisture[i]);
    setf("soilN", W().soilN[i]);
    setf("soilP", W().soilP[i]);
    setf("soilK", W().soilK[i]);
    setf("flowAccum", W().flowAccum[i]);
    setf("oreGrade", W().oreGrade[i] / 255.0);
    sets("biome", biomeName(static_cast<Biome>(W().biome[i])));
    sets("rock", rockName(static_cast<RockType>(W().strataRock[0][i])));
    sets("ore", oreName(static_cast<OreType>(W().oreType[i])));
    return 1;
}

int l_world_set_tile(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int x = static_cast<int>(luaL_checkinteger(L, 1));
    const int y = static_cast<int>(luaL_checkinteger(L, 2));
    const char* field = luaL_checkstring(L, 3);
    const double v = luaL_checknumber(L, 4);
    if (!W().inBounds(x, y)) return fail(L, "tile out of bounds");
    const size_t i = W().index(x, y);
    const float f = static_cast<float>(v);
    auto u8 = [&]() { return static_cast<uint8_t>(std::min(255.0, std::max(0.0, v))); };

    if      (!std::strcmp(field, "elevation"))    W().elevation[i] = f;
    else if (!std::strcmp(field, "water"))        W().waterDepth[i] = std::max(0.0f, f);
    else if (!std::strcmp(field, "temperature"))  W().temperature[i] = f;
    else if (!std::strcmp(field, "rainfall"))     W().rainfall[i] = std::max(0.0f, f);
    else if (!std::strcmp(field, "biomass"))      W().biomass[i] = std::max(0.0f, f);
    else if (!std::strcmp(field, "soilMoisture")) W().soilMoisture[i] = std::min(1.0f, std::max(0.0f, f));
    else if (!std::strcmp(field, "soilN"))        W().soilN[i] = u8();
    else if (!std::strcmp(field, "soilP"))        W().soilP[i] = u8();
    else if (!std::strcmp(field, "soilK"))        W().soilK[i] = u8();
    else if (!std::strcmp(field, "oreType"))      W().oreType[i] = u8();
    else if (!std::strcmp(field, "oreGrade"))     W().oreGrade[i] = u8();
    else if (!std::strcmp(field, "rock"))         W().strataRock[0][i] = u8();
    else return luaL_error(L, "unknown tile field '%s'", field);
    return 0;
}

// -- population -------------------------------------------------------------

int l_pop_count(lua_State* L) {
    if (!ctxReady(L)) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(A().population()));
    return 1;
}

int l_pop_uids(lua_State* L) {
    if (!ctxReady(L)) return 0;
    lua_newtable(L);
    int n = 0;
    for (uint32_t slot : A().liveSlots()) {
        lua_pushinteger(L, static_cast<lua_Integer>(A().m_uid[slot]));
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int l_agent_get(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    const uint32_t slot = static_cast<uint32_t>(s);
    const Phenotype& p = A().m_phenotype[slot];
    lua_newtable(L);
    auto setf = [&](const char* k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
    auto sets = [&](const char* k, const char* v) { lua_pushstring(L, v); lua_setfield(L, -2, k); };
    lua_pushinteger(L, static_cast<lua_Integer>(A().m_uid[slot]));
    lua_setfield(L, -2, "uid");
    sets("name", A().m_name[slot].c_str());
    setf("x", A().m_x[slot]);
    setf("y", A().m_y[slot]);
    setf("energy", A().m_energy[slot]);
    setf("hydration", A().m_hydration[slot]);
    setf("health", A().m_health[slot]);
    setf("stress", A().m_stress[slot]);
    setf("age", static_cast<double>(
        (g_ctx->tick > A().m_birthTick[slot] ? g_ctx->tick - A().m_birthTick[slot] : 0)
        + A().m_ageOffset[slot]) / static_cast<double>(kHoursPerYear));
    sets("stage", lifeStageName(static_cast<LifeStage>(A().m_stage[slot])));
    sets("action", actionName(static_cast<Action>(A().m_action[slot])));
    setf("sexExpression", p.get(Trait::SexExpression));
    setf("inbreedingF", p.inbreedingF);
    setf("heterozygosity", p.heterozygosity);
    lua_pushinteger(L, static_cast<lua_Integer>(A().m_motherUid[slot]));
    lua_setfield(L, -2, "mother");
    lua_pushinteger(L, static_cast<lua_Integer>(A().m_fatherUid[slot]));
    lua_setfield(L, -2, "father");
    lua_pushboolean(L, A().m_bondedUid[slot] != 0);
    lua_setfield(L, -2, "bonded");
    lua_pushboolean(L, A().immortal(slot));
    lua_setfield(L, -2, "immortal");
    return 1;
}

int l_agent_trait(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    const int t = traitIndexArg(L, 2);
    lua_pushnumber(L, A().m_phenotype[static_cast<uint32_t>(s)].traits[t]);
    return 1;
}

int l_agent_set_trait(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    const int t = traitIndexArg(L, 2);
    const float v = static_cast<float>(luaL_checknumber(L, 3));
    const TraitSpec& spec = traitSpec(static_cast<Trait>(t));
    const uint32_t slot = static_cast<uint32_t>(s);
    A().m_phenotype[slot].traits[t] = std::min(spec.maxValue, std::max(spec.minValue, v));
    buildPreferenceVector(A().m_phenotype[slot], A().m_prefs[slot]);
    return 0;
}

// Genome access from a script. The Genome Browser already lets every allele be
// edited from the UI; this is the same power through the console, which is what
// makes it possible to CONSTRUCT a divergent population rather than only wait
// for one to drift apart.
int l_agent_gene_count(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(A().genetics().genome(
        static_cast<size_t>(s)).count));
    return 1;
}

int l_agent_gene(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    const lua_Integer idx = luaL_checkinteger(L, 2);
    ConstGenomeView g = A().genetics().genome(static_cast<size_t>(s));
    if (idx < 1 || idx > static_cast<lua_Integer>(g.count))
        return luaL_error(L, "gene index %d out of range (1..%d)",
                          static_cast<int>(idx), static_cast<int>(g.count));
    const Gene& gene = g[static_cast<size_t>(idx - 1)];
    lua_createtable(L, 0, 9);
    auto num = [&](const char* k, double v) {
        lua_pushstring(L, k); lua_pushnumber(L, v); lua_settable(L, -3);
    };
    auto str = [&](const char* k, const char* v) {
        lua_pushstring(L, k); lua_pushstring(L, v); lua_settable(L, -3);
    };
    num("alleleA", gene.alleleA);
    num("alleleB", gene.alleleB);
    num("mapPos", gene.mapPos);
    num("effect", gene.effect);
    num("id", gene.id);
    num("target", gene.target);
    num("chromosome", gene.chromosome);
    num("type", gene.type);
    num("dominance", gene.dominance);
    str("typeName", locusTypeName(static_cast<LocusType>(gene.type)));
    return 1;
}

int l_agent_set_gene(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    const lua_Integer idx = luaL_checkinteger(L, 2);
    const char* field = luaL_checkstring(L, 3);
    const float v = static_cast<float>(luaL_checknumber(L, 4));
    GenomeView g = A().genetics().genome(static_cast<size_t>(s));
    if (idx < 1 || idx > static_cast<lua_Integer>(g.count))
        return luaL_error(L, "gene index %d out of range (1..%d)",
                          static_cast<int>(idx), static_cast<int>(g.count));
    Gene& gene = g[static_cast<size_t>(idx - 1)];
    if      (!std::strcmp(field, "alleleA")) gene.alleleA = v;
    else if (!std::strcmp(field, "alleleB")) gene.alleleB = v;
    else if (!std::strcmp(field, "effect"))  gene.effect = v;
    else if (!std::strcmp(field, "dominance"))
        gene.dominance = static_cast<uint8_t>(v);
    else return luaL_error(L, "unknown or read-only gene field '%s' -- writable: "
                              "alleleA, alleleB, effect, dominance", field);
    // Re-express, because an allele change has to propagate to the phenotype and
    // the brain the same way it does when the Genome Browser makes the edit.
    A().redevelop(static_cast<uint32_t>(s), *g_ctx->rng);
    return 0;
}

int l_agent_set(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    const uint32_t slot = static_cast<uint32_t>(s);
    const char* field = luaL_checkstring(L, 2);
    if (!std::strcmp(field, "name")) { A().m_name[slot] = luaL_checkstring(L, 3); return 0; }
    if (!std::strcmp(field, "immortal")) { A().setImmortal(slot, lua_toboolean(L, 3) != 0); return 0; }
    if (!std::strcmp(field, "tagged")) { A().setTagged(slot, lua_toboolean(L, 3) != 0); return 0; }
    const double v = luaL_checknumber(L, 3);
    const float f = static_cast<float>(v);
    if      (!std::strcmp(field, "x"))         A().m_x[slot] = f;
    else if (!std::strcmp(field, "y"))         A().m_y[slot] = f;
    else if (!std::strcmp(field, "energy"))    A().m_energy[slot] = f;
    else if (!std::strcmp(field, "hydration")) A().m_hydration[slot] = f;
    else if (!std::strcmp(field, "health"))    A().m_health[slot] = f;
    else if (!std::strcmp(field, "stress"))    A().m_stress[slot] = f;
    else return luaL_error(L, "unknown agent field '%s'", field);
    return 0;
}

int l_agent_kill(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t s = slotArg(L, 1);
    A().kill(static_cast<uint32_t>(s), DeathCause::Divine, g_ctx->tick, g_ctx->world);
    return 0;
}

int l_agent_attraction(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const int32_t a = slotArg(L, 1);
    const int32_t b = slotArg(L, 2);
    AttractionBreakdown br;
    const float v = A().attractionBetween(static_cast<uint32_t>(a), static_cast<uint32_t>(b), &br);
    lua_pushnumber(L, v);
    lua_pushboolean(L, br.wouldAccept);
    lua_pushnumber(L, br.threshold);
    return 3;
}

int l_agent_relatedness(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const lua_Integer ua = luaL_checkinteger(L, 1);
    const lua_Integer ub = luaL_checkinteger(L, 2);
    lua_pushnumber(L, A().pedigree().relatedness(static_cast<uint64_t>(ua),
                                                 static_cast<uint64_t>(ub)));
    return 1;
}

int l_agent_spawn(lua_State* L) {
    if (!ctxReady(L)) return 0;
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float y = static_cast<float>(luaL_checknumber(L, 2));
    const bool hetero = lua_toboolean(L, 3) != 0;
    const AgentId id = A().spawnFounder(x, y, *g_ctx->rng, g_ctx->tick, hetero);
    if (!id.valid()) return fail(L, "agent store is full");
    lua_pushinteger(L, static_cast<lua_Integer>(A().m_uid[id.slot]));
    return 1;
}

// -- god --------------------------------------------------------------------

int l_god_act(lua_State* L) {
    if (!ctxReady(L)) return 0;
    // god.act{kind="Wildfire", x=..., y=..., radius=..., intensity=..., f0=...}
    luaL_checktype(L, 1, LUA_TTABLE);
    GodAction a;
    lua_getfield(L, 1, "kind");
    const char* kindName = luaL_optstring(L, -1, "");
    lua_pop(L, 1);
    bool found = false;
    for (int k = 0; k < static_cast<int>(GodActionKind::Count); ++k) {
        if (std::strcmp(godActionName(static_cast<GodActionKind>(k)), kindName) == 0) {
            a.kind = static_cast<GodActionKind>(k);
            found = true;
            break;
        }
    }
    if (!found) return luaL_error(L, "unknown god action '%s'", kindName);

    auto num = [&](const char* k, float& dst) {
        lua_getfield(L, 1, k);
        if (lua_isnumber(L, -1)) dst = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    };
    auto inum = [&](const char* k, int32_t& dst) {
        lua_getfield(L, 1, k);
        if (lua_isnumber(L, -1)) dst = static_cast<int32_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    };
    num("x", a.x); num("y", a.y); num("radius", a.radius); num("intensity", a.intensity);
    num("f0", a.f0); num("f1", a.f1); num("f2", a.f2);
    inum("i0", a.i0); inum("i1", a.i1);
    lua_getfield(L, 1, "text");
    if (lua_isstring(L, -1)) a.text = lua_tostring(L, -1);
    lua_pop(L, 1);

    const std::string what = G().apply(a, W(), A(), *g_ctx->rng, g_ctx->tick);
    if (g_ctx->events) g_ctx->events->push_back(what);
    lua_pushstring(L, what.c_str());
    return 1;
}

int l_god_undo(lua_State* L) {
    if (!ctxReady(L)) return 0;
    std::string what;
    const bool ok = G().undo(W(), A(), g_ctx->tick, what);
    lua_pushboolean(L, ok);
    lua_pushstring(L, what.c_str());
    return 2;
}

// -- config -----------------------------------------------------------------

int l_config_get(lua_State* L) {
    lua_pushnumber(L, cfg().getFloat(luaL_checkstring(L, 1)));
    return 1;
}

int l_config_set(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    // A boolean setting is stored as a float, but writing `true` to it from a
    // script should work rather than raise a type error -- the caller is right
    // and the storage is an implementation detail.
    if (lua_isboolean(L, 2)) {
        cfg().setFloat(key, lua_toboolean(L, 2) ? 1.0 : 0.0);
        return 0;
    }
    cfg().setFloat(key, luaL_checknumber(L, 2));
    return 0;
}

int l_config_keys(lua_State* L) {
    lua_newtable(L);
    int n = 0;
    for (const CfgEntry& e : cfg().entries()) {
        lua_pushstring(L, e.key.c_str());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// -- output -----------------------------------------------------------------

int l_print(lua_State* L) {
    const int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += "\t";
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        line.append(s, len);
        lua_pop(L, 1);
    }
    if (g_console) g_console->addLine(line, 3);
    return 0;
}

// The instruction-count hook. A script runs with the world lock held, so an
// unbounded loop would hang the simulation; this stops it instead.
void countHook(lua_State* L, lua_Debug*) {
    luaL_error(L, "script exceeded its instruction budget (lua.instruction_budget)");
}

struct ApiEntry { const char* name; const char* help; };
const ApiEntry kApi[] = {
    {"world.size()", "Returns width, height in tiles."},
    {"world.tick()", "Current simulation tick (one tick = one simulated hour)."},
    {"world.date()", "Current date as a string."},
    {"world.tile(x, y)", "Table of every field of one tile."},
    {"world.setTile(x, y, field, value)", "Sets one tile field: elevation, water, temperature, rainfall, biomass, soilMoisture, soilN/P/K, oreType, oreGrade, rock."},
    {"pop.count()", "Number of living agents."},
    {"pop.uids()", "Array of every living agent's uid."},
    {"agent.get(uid)", "Table of an individual's state."},
    {"agent.set(uid, field, value)", "Sets x, y, energy, hydration, health, stress, name, immortal or tagged."},
    {"agent.trait(uid, trait)", "Reads one expressed trait by name or index."},
    {"agent.setTrait(uid, trait, value)", "Writes one expressed trait, clamped to its range."},
    {"agent.kill(uid)", "Divine death. Bypasses immortality."},
    {"agent.spawn(x, y, heterogametic)", "Creates a founder-style adult; returns its uid."},
    {"agent.attraction(a, b)", "Returns score, wouldAccept, threshold for a -> b."},
    {"agent.relatedness(uidA, uidB)", "Wright's coefficient of relatedness from the pedigree."},
    {"agent.geneCount(uid)", "How many genes this individual's genome holds."},
    {"agent.gene(uid, i)", "Table of gene i (1-based): alleleA, alleleB, mapPos, effect, id, target, chromosome, type, dominance, typeName."},
    {"agent.setGene(uid, i, field, value)", "Writes alleleA, alleleB, effect or dominance, then re-expresses the phenotype and rebuilds the brain."},
    {"god.act{kind=..., x=, y=, radius=, intensity=, f0=, i0=}", "Applies any god action by name; undoable like any other."},
    {"god.undo()", "Undoes the most recent divine act."},
    {"config.get(key)", "Reads any tunable constant."},
    {"config.set(key, value)", "Writes any tunable constant."},
    {"config.keys()", "Array of every registered config key."},
    {"print(...)", "Writes to the console output."},
};

}  // namespace

// ---------------------------------------------------------------------------

LuaConsole::LuaConsole() = default;
LuaConsole::~LuaConsole() { shutdown(); }

void LuaConsole::addLine(const std::string& text, int kind) {
    LuaLine l;
    l.text = text;
    l.kind = kind;
    m_lines.push_back(std::move(l));
    while (m_lines.size() > 2000) m_lines.pop_front();
}

bool LuaConsole::init() {
    if (m_lua) return true;
    m_lua = luaL_newstate();
    if (!m_lua) return false;
    // Base, string, table and math only. io and os are deliberately left out:
    // a console that can delete files is not a simulation console.
    luaL_requiref(m_lua, LUA_GNAME, luaopen_base, 1);         lua_pop(m_lua, 1);
    luaL_requiref(m_lua, LUA_TABLIBNAME, luaopen_table, 1);   lua_pop(m_lua, 1);
    luaL_requiref(m_lua, LUA_STRLIBNAME, luaopen_string, 1);  lua_pop(m_lua, 1);
    luaL_requiref(m_lua, LUA_MATHLIBNAME, luaopen_math, 1);   lua_pop(m_lua, 1);

    // Remove the parts of the base library that reach outside the process.
    const char* strip[] = {"dofile", "loadfile", "load", "collectgarbage", "require"};
    for (const char* n : strip) {
        lua_pushnil(m_lua);
        lua_setglobal(m_lua, n);
    }

    registerApi();
    buildCompletions();
    addLine("Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
            " ready. Type help() for the API, or a Lua expression.", 1);
    return true;
}

void LuaConsole::shutdown() {
    if (m_lua) {
        lua_close(m_lua);
        m_lua = nullptr;
    }
}

namespace {
int l_help(lua_State* L) {
    if (!g_console) return 0;
    const char* filter = luaL_optstring(L, 1, "");
    for (const ApiEntry& e : kApi) {
        if (*filter && !std::strstr(e.name, filter)) continue;
        g_console->addLine(std::string(e.name) + "  --  " + e.help, 1);
    }
    return 0;
}
}  // namespace

void LuaConsole::registerApi() {
    lua_State* L = m_lua;
    auto table = [&](const char* name, const luaL_Reg* fns) {
        lua_newtable(L);
        for (const luaL_Reg* f = fns; f->name; ++f) {
            lua_pushcfunction(L, f->func);
            lua_setfield(L, -2, f->name);
        }
        lua_setglobal(L, name);
    };

    static const luaL_Reg worldFns[] = {
        {"size", l_world_size}, {"tick", l_world_tick}, {"date", l_world_date},
        {"tile", l_world_tile}, {"setTile", l_world_set_tile}, {nullptr, nullptr}};
    static const luaL_Reg popFns[] = {
        {"count", l_pop_count}, {"uids", l_pop_uids}, {nullptr, nullptr}};
    static const luaL_Reg agentFns[] = {
        {"get", l_agent_get}, {"set", l_agent_set}, {"trait", l_agent_trait},
        {"setTrait", l_agent_set_trait}, {"kill", l_agent_kill}, {"spawn", l_agent_spawn},
        {"attraction", l_agent_attraction}, {"relatedness", l_agent_relatedness},
        {"geneCount", l_agent_gene_count}, {"gene", l_agent_gene},
        {"setGene", l_agent_set_gene},
        {nullptr, nullptr}};
    static const luaL_Reg godFns[] = {
        {"act", l_god_act}, {"undo", l_god_undo}, {nullptr, nullptr}};
    static const luaL_Reg configFns[] = {
        {"get", l_config_get}, {"set", l_config_set}, {"keys", l_config_keys},
        {nullptr, nullptr}};

    table("world", worldFns);
    table("pop", popFns);
    table("agent", agentFns);
    table("god", godFns);
    table("config", configFns);

    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, l_help);
    lua_setglobal(L, "help");

    // Trait and god-action names, so scripts can enumerate them.
    lua_newtable(L);
    for (int t = 0; t < kTraitCount; ++t) {
        lua_pushstring(L, traitSpec(static_cast<Trait>(t)).name);
        lua_rawseti(L, -2, t + 1);
    }
    lua_setglobal(L, "traits");

    lua_newtable(L);
    for (int k = 1; k < static_cast<int>(GodActionKind::Count); ++k) {
        lua_pushstring(L, godActionName(static_cast<GodActionKind>(k)));
        lua_rawseti(L, -2, k);
    }
    lua_setglobal(L, "godActions");
}

void LuaConsole::buildCompletions() {
    m_completions.clear();
    for (const ApiEntry& e : kApi) {
        std::string n = e.name;
        const size_t paren = n.find('(');
        m_completions.push_back(paren == std::string::npos ? n : n.substr(0, paren));
    }
    m_completions.push_back("help");
    m_completions.push_back("traits");
    m_completions.push_back("godActions");
    for (int t = 0; t < kTraitCount; ++t)
        m_completions.push_back(std::string("\"") + traitSpec(static_cast<Trait>(t)).name + "\"");
    std::sort(m_completions.begin(), m_completions.end());
    m_completions.erase(std::unique(m_completions.begin(), m_completions.end()),
                        m_completions.end());
}

void LuaConsole::complete(const std::string& prefix, std::vector<std::string>& out) const {
    out.clear();
    if (prefix.empty()) return;
    for (const std::string& c : m_completions)
        if (c.size() >= prefix.size() && c.compare(0, prefix.size(), prefix) == 0)
            out.push_back(c);
}

const char* LuaConsole::helpFor(const std::string& name) const {
    for (const ApiEntry& e : kApi) {
        std::string n = e.name;
        const size_t paren = n.find('(');
        if (paren != std::string::npos) n = n.substr(0, paren);
        if (n == name) return e.help;
    }
    return nullptr;
}

void LuaConsole::run(const std::string& source, LuaContext& ctx) {
    if (!m_lua) { addLine("Lua is not initialised", 2); return; }

    g_ctx = &ctx;
    g_console = this;

    addLine("> " + source, 0);

    // An expression is far more useful at a console than a statement, so try to
    // wrap it in a return first and fall back to running it as a chunk.
    const std::string asExpression = "return " + source;
    int status = luaL_loadstring(m_lua, asExpression.c_str());
    if (status != LUA_OK) {
        lua_pop(m_lua, 1);
        status = luaL_loadstring(m_lua, source.c_str());
    }

    if (status == LUA_OK) {
        const int budget = static_cast<int>(cfg().getInt("lua.instruction_budget", 20000000));
        lua_sethook(m_lua, countHook, LUA_MASKCOUNT, budget);
        const int base = lua_gettop(m_lua) - 1;
        status = lua_pcall(m_lua, 0, LUA_MULTRET, 0);
        lua_sethook(m_lua, nullptr, 0, 0);

        if (status == LUA_OK) {
            const int nres = lua_gettop(m_lua) - base;
            for (int i = 1; i <= nres; ++i) {
                size_t len = 0;
                const char* s = luaL_tolstring(m_lua, base + i, &len);
                addLine(std::string(s, len), 1);
                lua_pop(m_lua, 1);
            }
            lua_settop(m_lua, base);
        }
    }

    if (status != LUA_OK) {
        const char* msg = lua_tostring(m_lua, -1);
        addLine(msg ? msg : "unknown error", 2);
        lua_pop(m_lua, 1);
    }

    g_ctx = nullptr;
    g_console = nullptr;
}

}  // namespace gen
