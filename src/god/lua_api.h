// god/lua_api.h — the embedded Lua 5.4 console.
//
// The console is the escape hatch for everything the buttons do not cover. It
// runs on the SIM THREAD at a tick boundary with the world lock held
// exclusively, exactly like any other divine act, so a script can read and
// mutate simulation state without racing a tick.
//
// A script therefore blocks the simulation while it runs. That is the correct
// trade -- the alternative is a script observing a world that changes under it
// mid-statement -- but it means a runaway loop would hang the sim, so execution
// is bounded by an instruction-count hook (`lua.instruction_budget`).
#pragma once

#include <deque>
#include <string>
#include <vector>

struct lua_State;

namespace gen {

class World;
class Agents;
class GodMode;
class RngBank;

struct LuaLine {
    std::string text;
    int  kind = 0;   // 0 = echo of input, 1 = result, 2 = error, 3 = print output
};

// Everything a script is allowed to touch, bound for the duration of one run.
struct LuaContext {
    World*   world = nullptr;
    Agents*  agents = nullptr;
    GodMode* god = nullptr;
    RngBank* rng = nullptr;
    uint64_t tick = 0;
    std::vector<std::string>* events = nullptr;   // messages the script emitted
};

class LuaConsole {
public:
    LuaConsole();
    ~LuaConsole();

    LuaConsole(const LuaConsole&) = delete;
    LuaConsole& operator=(const LuaConsole&) = delete;

    bool init();
    void shutdown();
    bool ready() const { return m_lua != nullptr; }

    // Runs a chunk with `ctx` bound. Called ONLY from the sim thread.
    void run(const std::string& source, LuaContext& ctx);

    // Output ring, read by the UI.
    const std::deque<LuaLine>& lines() const { return m_lines; }
    void clearLines() { m_lines.clear(); }
    void addLine(const std::string& text, int kind);

    // Every name the API exposes, for autocomplete and for the reference list.
    const std::vector<std::string>& completions() const { return m_completions; }
    // Candidates that share a prefix with `prefix`, best-first.
    void complete(const std::string& prefix, std::vector<std::string>& out) const;
    // One-line documentation for an API name, or nullptr.
    const char* helpFor(const std::string& name) const;

private:
    void registerApi();
    void buildCompletions();

    lua_State* m_lua = nullptr;
    std::deque<LuaLine> m_lines;
    std::vector<std::string> m_completions;
};

}  // namespace gen
