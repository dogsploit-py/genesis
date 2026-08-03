// sim/simulation.h — the simulation host: owns the world, runs the sim thread,
// and is the only legal channel between the UI thread and simulation state.
//
// The ownership rule (ARCHITECTURE.md §1): the sim thread owns World. The UI
// never mutates it and only reads it under a shared lock. Every UI-originated
// change is a Command applied at the top of a numbered tick, which is what
// makes god-mode interventions deterministic, loggable and replayable.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/jobs.h"
#include "core/profiler.h"
#include "econ/economy.h"
#include "core/rng.h"
#include "god/god.h"
#include "god/lua_api.h"
#include "sim/agent.h"
#include "sim/time.h"
#include "sim/world.h"

namespace gen {

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

enum class CommandType : uint16_t {
    None = 0,
    SetPaused,
    SetSpeed,          // a = hours per real second
    SetMaxSpeed,
    StepTicks,         // a = number of ticks
    SetRunUntil,       // ix = RunUntilKind, a = value, text = label
    CancelRunUntil,
    RegenerateWorld,   // a = seed (or -1 to keep), ix/iy = size
    SetConfigValue,    // text = key, a = value
    SaveSnapshot,      // text = path
    LoadSnapshot,      // text = path
    AddBookmark,       // text = name
    JumpToBookmark,    // ix = bookmark index
    ExportTelemetry,   // text = path
    EditAgent,         // a deferred closure applied on the sim thread
    SpawnAgents,       // ix = count, a = x, b = y
    GodAct,            // a queued GodAction (see god/god.h)
    Undo,
    Redo,
    CastMiracle,       // ix = miracle index
    RunScript,         // text = Lua source
    Count
};

const char* commandName(CommandType t);

struct Command {
    CommandType type = CommandType::None;
    double      a = 0.0, b = 0.0;
    int32_t     ix = 0, iy = 0;
    std::string text;

    // Filled in by the sim thread when the command is applied.
    uint64_t appliedTick = 0;
};

// Whether a command counts as divine intervention (and therefore belongs in the
// "how much did I cheat" audit) or is merely a view/pacing control. Pausing the
// world is not cheating; editing an allele is.
bool isIntervention(CommandType t);

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

enum class EventKind : uint8_t {
    Info = 0, WorldCreated, Divine, Disaster, Discovery,
    Birth, Death, Extinction, Fixation, Epidemic, War, Emergence,
    Count
};

const char* eventKindName(EventKind k);

struct WorldEvent {
    uint64_t  tick = 0;
    EventKind kind = EventKind::Info;
    int32_t   x = -1, y = -1;      // world location, -1 if not spatial
    uint64_t  subject = 0xFFFFFFFFFFFFFFFFull;  // agent uid, or all-ones for none
    std::string text;
};

// ---------------------------------------------------------------------------
// Telemetry: named time series, sampled on the telemetry cadence.
// ---------------------------------------------------------------------------

struct Series {
    std::string           key;
    std::string           label;
    std::string           unit;
    std::vector<uint64_t> ticks;
    std::vector<float>    values;
};

// ---------------------------------------------------------------------------
// What the UI is allowed to see without locking the world.
// ---------------------------------------------------------------------------

struct SimSnapshot {
    uint64_t   tick = 0;
    DateTime   date;
    Season     season = Season::Spring;
    double     simYears = 0.0;

    double     ticksPerSecond = 0.0;
    double     effectiveMultiplier = 0.0;   // achieved sim hours per real second
    double     requestedSpeed = 1.0;
    bool       paused = true;
    bool       maxSpeed = false;
    const char* bottleneck = "idle";
    double     lastBatchMs = 0.0;
    uint64_t   batchSize = 0;

    double     realElapsedSeconds = 0.0;
    WorldStats world;
    uint64_t   agentCount = 0;
    PopulationStats population;
    Genetics::PopulationGenetics popGenetics;
    double     fst = 0.0;

    bool        runUntilActive = false;
    double      runUntilProgress = 0.0;
    std::string runUntilLabel;

    uint32_t    undoDepth = 0, redoDepth = 0;
    double      undoMemoryMb = 0.0;
    uint32_t    activeDisasters = 0;
    float       temperatureOffset = 0.0f;
    float       rainfallMultiplier = 1.0f;

    bool        worldReady = false;
    float       genProgress = 1.0f;
    std::string genStage;

    uint64_t    seed = 0;
    uint32_t    workerCount = 0;
    uint64_t    interventionCount = 0;

    // Per-stage timings, copied out of the profiler under the snapshot lock.
    // Not simulation state: the profiler reads nothing and consumes no
    // randomness, so it cannot change a run.
    double      stageMeanMs[static_cast<int>(Stage::Count)] = {};
    double      stagePeakMs[static_cast<int>(Stage::Count)] = {};
    double      stageTotalMs[static_cast<int>(Stage::Count)] = {};
    double      tickMeanMs = 0.0;
    uint64_t    profiledTicks = 0;

    // Economy. All zero and all meaningless while `economyActive` is false,
    // which is the default and stays the default unless someone turns it on.
    bool        economyActive = false;
    bool        hasCurrency = false;
    std::string currencyName;
    uint64_t    totalTrades = 0;
    double      coincidenceRate = 0.0;
    double      gini = 0.0;
};

// ---------------------------------------------------------------------------

class Simulation {
public:
    Simulation() = default;
    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // Creates the job system and the world, then starts the sim thread.
    void start(const WorldParams& params);
    void stop();
    bool running() const { return m_running.load(std::memory_order_relaxed); }

    // -- UI -> sim -----------------------------------------------------------
    void push(const Command& c);
    // Queues a god action. Applied on the sim thread at a tick boundary with
    // the world lock held exclusively, then pushed onto the undo stack and
    // written to the intervention log.
    void pushGodAction(const GodAction& a);
    void push(CommandType t) { Command c; c.type = t; push(c); }
    void pushSpeed(double hoursPerSecond);
    void pushPause(bool paused);
    void pushStep(uint64_t ticks);

    // -- sim -> UI -----------------------------------------------------------
    SimSnapshot snapshot() const;
    // The profiler is not simulation state, so it is read directly rather
    // than through the snapshot double-buffer.
    // The economy. Read directly rather than through the snapshot, because
    // while it is inactive there is nothing in it to copy.
    const Economy& economy() const { return m_economy; }
    Economy& economy() { return m_economy; }

    const Profiler& profiler() const { return m_profiler; }
    Profiler& profiler() { return m_profiler; }

    // Runs fn(const World&) while holding the world read lock. Keep it short:
    // this is the only place the UI can stall the sim.
    template <typename Fn>
    void readWorld(Fn&& fn) const {
        std::shared_lock<std::shared_mutex> lk(m_worldMutex);
        fn(m_world);
    }

    // Same contract for the population. Separate from readWorld so a panel that
    // only needs agents does not also block on world access.
    template <typename Fn>
    void readAgents(Fn&& fn) const {
        std::shared_lock<std::shared_mutex> lk(m_worldMutex);
        fn(m_agents);
    }

    // God-mode state is read directly by the UI under the same shared lock.
    template <typename Fn>
    void readGod(Fn&& fn) const {
        std::shared_lock<std::shared_mutex> lk(m_worldMutex);
        fn(m_god);
    }
    // Miracles are edited from the UI; the edit is applied on the sim thread.
    void editGod(std::function<void(GodMode&)> fn, const std::string& description);

    // Queues a Lua chunk. It runs on the SIM THREAD at a tick boundary with
    // the world lock held exclusively, so a script sees a coherent world --
    // at the cost of blocking the simulation while it runs, which is why
    // execution is bounded by an instruction budget.
    void pushScript(const std::string& source);
    // Console output and completions, read by the UI between frames.
    template <typename Fn>
    void readLua(Fn&& fn) const {
        std::lock_guard<std::mutex> lk(m_luaMutex);
        fn(m_lua);
    }
    void clearLuaOutput();

    // Mutating access, applied at a tick boundary. Used by the Individual Card
    // and the god toolbar: the closure runs on the SIM thread between ticks
    // with the world lock held exclusively, so an edit can never tear state or
    // race a tick. Every call is recorded in the intervention log.
    void editAgents(std::function<void(Agents&, RngBank&)> fn, const std::string& description);

    // Copies the most recent `maxCount` events, newest last.
    void copyEvents(std::vector<WorldEvent>& out, size_t maxCount) const;
    size_t eventCount() const;

    void copySeriesKeys(std::vector<std::string>& keys, std::vector<std::string>& labels) const;
    bool copySeries(const std::string& key, Series& out) const;

    void copyInterventionLog(std::vector<Command>& out) const;
    void copyBookmarks(std::vector<Bookmark>& out) const;

    // -- direct calls, safe from any thread ---------------------------------
    bool saveSnapshotNow(const std::string& path);
    // Only safe to call while the sim thread is stopped (headless, or before
    // start()). From a running simulation, push a LoadSnapshot command instead
    // so the load happens at a tick boundary.
    bool loadSnapshotNow(const std::string& path);
    bool exportTelemetryCsv(const std::string& path) const;
    const std::string& lastError() const;

    // -- headless ------------------------------------------------------------
    // Runs `years` simulated years on the calling thread with no window, then
    // writes a snapshot and CSV telemetry. Returns false on failure.
    // If `loadPath` is non-empty the world is restored from it and the run
    // continues from that tick, instead of generating a fresh world.
    // A Lua chunk run once in headless mode, after the world is built and
    // before the run starts. This is how god mode is exercised without a GUI,
    // and it makes headless runs scriptable in their own right.
    void setStartupScript(const std::string& s) { m_startupScript = s; }

    bool runHeadless(const WorldParams& params, double years,
                     const std::string& snapshotPath, const std::string& csvPath,
                     bool verbose, const std::string& loadPath = std::string());

private:
    void threadMain();
    void tickOnce();                 // exactly one simulated hour
    void runBatch(uint64_t count);
    void drainCommands();
    void applyCommand(Command& c);
    void publishSnapshot();
    void sampleTelemetry();
    void checkRunUntil();
    void logEvent(EventKind kind, const std::string& text, int32_t x = -1, int32_t y = -1,
                  uint64_t subject = 0xFFFFFFFFFFFFFFFFull);
    void addSeriesPoint(const char* key, const char* label, const char* unit, float v);
    void regenerate(uint64_t seed, int w, int h);
    bool loadSnapshotInternal(const std::string& path);
    bool saveSnapshotInternal(const std::string& path);

    // Owned by the sim thread.
    World      m_world;
    Agents     m_agents;
    GodMode    m_god;
    LuaConsole m_lua;
    RngBank    m_rng;
    JobSystem  m_jobs;
    SpeedController m_speed;
    RunUntil   m_runUntil;
    uint64_t   m_tick = 0;
    const char* m_bottleneck = "idle";
    // True only inside runHeadless with verbose output, so milestone events can
    // be printed as they happen.
    bool        m_headlessVerbose = false;
    Profiler    m_profiler;
    Economy     m_economy;
    uint64_t   m_adaptiveBatch = 1;
    uint64_t   m_lastAutosaveYear = 0;
    bool       m_extinctionLogged = false;

public:
    // Headless diagnostic: print the first living agent's state every N ticks.
    // 0 disables. Set from --trace-agent.
    uint64_t   m_traceAgentPeriod = 0;
    uint64_t   m_traceAgentUid = 1;
    // A Lua chunk run once, after generation, in headless mode.
    std::string m_startupScript;

private:

    mutable std::shared_mutex m_worldMutex;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_quit{false};

    mutable std::mutex   m_cmdMutex;
    std::vector<Command> m_cmdQueue;

    // Deferred edits from the UI, drained at the same tick boundary as commands.
    struct PendingEdit { std::function<void(Agents&, RngBank&)> fn; std::string description; };
    mutable std::mutex       m_editMutex;
    std::vector<PendingEdit> m_editQueue;

    struct PendingGodEdit { std::function<void(GodMode&)> fn; std::string description; };
    mutable std::mutex           m_luaMutex;
    std::vector<std::string>     m_scriptQueue;
    mutable std::mutex           m_godMutex;
    std::vector<GodAction>       m_godQueue;
    std::vector<PendingGodEdit>  m_godEditQueue;

    mutable std::mutex      m_eventMutex;
    std::deque<WorldEvent>  m_events;

    mutable std::mutex      m_telemetryMutex;
    std::vector<Series>     m_series;

    mutable std::mutex      m_snapshotMutex;
    SimSnapshot             m_snapshot;

    mutable std::mutex      m_logMutex;
    std::vector<Command>    m_interventionLog;
    std::vector<Bookmark>   m_bookmarks;

    mutable std::mutex      m_errorMutex;
    std::string             m_lastError;

    // Timing, sim thread only.
    double m_realElapsed = 0.0;
    double m_ticksPerSecond = 0.0;
    double m_lastBatchMs = 0.0;
    double m_windowTicks = 0.0;
    double m_windowSeconds = 0.0;
};

}  // namespace gen
