#include "sim/simulation.h"

#include "chem/reactions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "core/serialize.h"

namespace gen {

namespace {
using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Generation progress is reported through a file-scope hook because World's
// callback is a plain function pointer (deliberately, to keep World free of
// std::function in its interface). The sim thread is the only writer.
struct GenProgressSink {
    Simulation* sim = nullptr;
    float       value = 0.0f;
    std::string stage;
    std::mutex  mutex;
};
GenProgressSink g_genProgress;

void genProgressCallback(float t, const char* stage, void* user) {
    (void)user;
    std::lock_guard<std::mutex> lk(g_genProgress.mutex);
    g_genProgress.value = t;
    g_genProgress.stage = stage ? stage : "";
}
}  // namespace

const char* commandName(CommandType t) {
    switch (t) {
        case CommandType::None:            return "none";
        case CommandType::SetPaused:       return "Set paused";
        case CommandType::SetSpeed:        return "Set speed";
        case CommandType::SetMaxSpeed:     return "Set MAX speed";
        case CommandType::StepTicks:       return "Step ticks";
        case CommandType::SetRunUntil:     return "Run until";
        case CommandType::CancelRunUntil:  return "Cancel run-until";
        case CommandType::RegenerateWorld: return "Regenerate world";
        case CommandType::SetConfigValue:  return "Set config value";
        case CommandType::SaveSnapshot:    return "Save snapshot";
        case CommandType::LoadSnapshot:    return "Load snapshot";
        case CommandType::AddBookmark:     return "Add bookmark";
        case CommandType::JumpToBookmark:  return "Jump to bookmark";
        case CommandType::ExportTelemetry: return "Export telemetry";
        case CommandType::EditAgent:       return "Edit individual";
        case CommandType::SpawnAgents:     return "Spawn agents";
        case CommandType::GodAct:          return "Divine act";
        case CommandType::Undo:            return "Undo";
        case CommandType::Redo:            return "Redo";
        case CommandType::CastMiracle:     return "Cast miracle";
        case CommandType::RunScript:       return "Lua script";
        case CommandType::Count:           break;
    }
    return "?";
}

// Pacing and view controls are not interventions. Anything that changes what
// the world would have done on its own is.
bool isIntervention(CommandType t) {
    switch (t) {
        case CommandType::SetPaused:
        case CommandType::SetSpeed:
        case CommandType::SetMaxSpeed:
        case CommandType::StepTicks:
        case CommandType::SetRunUntil:
        case CommandType::CancelRunUntil:
        case CommandType::SaveSnapshot:
        case CommandType::ExportTelemetry:
        case CommandType::AddBookmark:
        case CommandType::None:
            return false;
        // Undo and redo ARE logged: how much you took back is part of how much
        // you intervened, and hiding it would make the audit trail a lie.
        default:
            return true;
    }
}

const char* eventKindName(EventKind k) {
    switch (k) {
        case EventKind::Info:         return "Info";
        case EventKind::WorldCreated: return "World";
        case EventKind::Divine:       return "Divine";
        case EventKind::Disaster:     return "Disaster";
        case EventKind::Discovery:    return "Discovery";
        case EventKind::Birth:        return "Birth";
        case EventKind::Death:        return "Death";
        case EventKind::Extinction:   return "Extinction";
        case EventKind::Fixation:     return "Fixation";
        case EventKind::Epidemic:     return "Epidemic";
        case EventKind::War:          return "War";
        case EventKind::Emergence:    return "Emergence";
        case EventKind::Count:        break;
    }
    return "?";
}

// ---------------------------------------------------------------------------

Simulation::~Simulation() { stop(); }

void Simulation::start(const WorldParams& params) {
    stop();

    // God mode reaches the economy through a pointer it does not own, so the
    // optional module stays optional.
    m_god.setEconomy(&m_economy);

    unsigned workers = static_cast<unsigned>(cfg().getInt("sim.worker_threads", 0));
    if (workers == 0) workers = JobSystem::recommendedWorkers();
    m_jobs.start(workers);

    m_quit.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);

    g_genProgress.sim = this;
    m_thread = std::thread([this, params] {
        regenerate(params.seed, params.width, params.height);
        threadMain();
    });
}

void Simulation::stop() {
    if (!m_thread.joinable()) { m_running.store(false, std::memory_order_relaxed); return; }
    m_quit.store(true, std::memory_order_relaxed);
    m_thread.join();
    m_running.store(false, std::memory_order_relaxed);
    m_jobs.shutdown();
}

void Simulation::push(const Command& c) {
    std::lock_guard<std::mutex> lk(m_cmdMutex);
    m_cmdQueue.push_back(c);
}

void Simulation::pushSpeed(double hoursPerSecond) {
    Command c;
    c.type = CommandType::SetSpeed;
    c.a = hoursPerSecond;
    push(c);
}

void Simulation::pushPause(bool paused) {
    Command c;
    c.type = CommandType::SetPaused;
    c.a = paused ? 1.0 : 0.0;
    push(c);
}

void Simulation::pushStep(uint64_t ticks) {
    Command c;
    c.type = CommandType::StepTicks;
    c.a = static_cast<double>(ticks);
    push(c);
}

SimSnapshot Simulation::snapshot() const {
    std::lock_guard<std::mutex> lk(m_snapshotMutex);
    return m_snapshot;
}

void Simulation::copyEvents(std::vector<WorldEvent>& out, size_t maxCount) const {
    std::lock_guard<std::mutex> lk(m_eventMutex);
    const size_t n = m_events.size();
    const size_t take = (maxCount < n) ? maxCount : n;
    out.clear();
    out.reserve(take);
    for (size_t i = n - take; i < n; ++i) out.push_back(m_events[i]);
}

size_t Simulation::eventCount() const {
    std::lock_guard<std::mutex> lk(m_eventMutex);
    return m_events.size();
}

void Simulation::copySeriesKeys(std::vector<std::string>& keys,
                                std::vector<std::string>& labels) const {
    std::lock_guard<std::mutex> lk(m_telemetryMutex);
    keys.clear();
    labels.clear();
    for (const Series& s : m_series) { keys.push_back(s.key); labels.push_back(s.label); }
}

bool Simulation::copySeries(const std::string& key, Series& out) const {
    std::lock_guard<std::mutex> lk(m_telemetryMutex);
    for (const Series& s : m_series) {
        if (s.key == key) { out = s; return true; }
    }
    return false;
}

void Simulation::copyInterventionLog(std::vector<Command>& out) const {
    std::lock_guard<std::mutex> lk(m_logMutex);
    out = m_interventionLog;
}

void Simulation::copyBookmarks(std::vector<Bookmark>& out) const {
    std::lock_guard<std::mutex> lk(m_logMutex);
    out = m_bookmarks;
}

const std::string& Simulation::lastError() const {
    // Returning a reference to a member guarded by a mutex is safe here because
    // the string is only ever replaced under that mutex and read on the UI
    // thread between frames; the copy is made by the caller immediately.
    std::lock_guard<std::mutex> lk(m_errorMutex);
    return m_lastError;
}

// ---------------------------------------------------------------------------
// Sim thread
// ---------------------------------------------------------------------------

void Simulation::regenerate(uint64_t seed, int w, int h) {
    std::unique_lock<std::shared_mutex> lk(m_worldMutex);

    WorldParams p;
    p.width = w;
    p.height = h;
    p.seed = seed;
    p.tileMetres   = cfg().getF("world.tile_metres", 50.0f);
    p.seaLevel     = cfg().getF("world.sea_level", 0.0f);
    p.latitudeSpan = cfg().getF("world.latitude_span", 120.0f);

    m_rng.reseed(seed);
    m_tick = 0;
    m_lastAutosaveYear = 0;
    m_extinctionLogged = false;
    m_runUntil.clear();
    m_god.configure();
    m_god.clearHistory();
    m_god.disasters().clear();

    {
        std::lock_guard<std::mutex> gl(g_genProgress.mutex);
        g_genProgress.value = 0.0f;
        g_genProgress.stage = "Starting";
    }
    // Publish "not ready" so the UI shows the generation bar rather than an
    // empty viewport.
    {
        std::lock_guard<std::mutex> sl(m_snapshotMutex);
        m_snapshot.worldReady = false;
        m_snapshot.genProgress = 0.0f;
        m_snapshot.genStage = "Starting";
        m_snapshot.seed = seed;
    }

    m_world.generate(p, m_rng, m_jobs, &genProgressCallback, nullptr);

    // -- founding population ------------------------------------------------
    m_agents.configure(static_cast<size_t>(cfg().getInt("sim.max_agents", 12000)),
                       m_rng[Stream::Genetics]);

    const int founderCount = static_cast<int>(cfg().getInt("sim.founder_count", 20));
    const float spread = cfg().getF("sim.founder_spread", 24.0f);
    if (founderCount > 0) {
        // Find somewhere they can actually survive: land, with standing plant
        // biomass and water within reach. Dropping founders in a desert is a
        // valid experiment but a poor default.
        Rng& r = m_rng[Stream::WorldGen];
        int bestX = m_world.width() / 2, bestY = m_world.height() / 2;
        float bestScore = -1e30f;
        const int waterSearch = 6;   // tiles
        for (int attempt = 0; attempt < 6000; ++attempt) {
            const int x = r.range(0, m_world.width());
            const int y = r.range(0, m_world.height());
            const size_t i = m_world.index(x, y);
            if (m_world.elevation[i] <= m_world.params().seaLevel) continue;
            if (m_world.waterDepth[i] > 0.5f) continue;

            // Drinkable water must actually be reachable. Founders with no
            // water within a few tiles dehydrate in about ten days no matter
            // how good the forage is, so this is a hard requirement rather
            // than a scoring bonus.
            bool waterNearby = false;
            for (int dy = -waterSearch; dy <= waterSearch && !waterNearby; ++dy) {
                for (int dx = -waterSearch; dx <= waterSearch && !waterNearby; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (!m_world.inBounds(nx, ny)) continue;
                    if (m_world.waterDepth[m_world.index(nx, ny)] > 0.02f) waterNearby = true;
                }
            }
            if (!waterNearby) continue;

            float score = m_world.biomass[i];
            // Prefer somewhere temperate: a founding population that freezes in
            // its first winter teaches nothing.
            score -= std::fabs(m_world.temperature[i] - 18.0f) * 8.0f;
            if (score > bestScore) { bestScore = score; bestX = x; bestY = y; }
        }
        if (bestScore <= -1e29f)
            logEvent(EventKind::Info,
                     "No land tile with water within reach was found; founders placed at the "
                     "map centre and will probably not survive.");

        int placed = 0;
        for (int k = 0; k < founderCount; ++k) {
            // Scattering blindly around the centre drops founders into the sea,
            // where they cannot walk out and simply starve. Each position is
            // retried until it lands on walkable ground.
            float fx = static_cast<float>(bestX), fy = static_cast<float>(bestY);
            for (int attempt = 0; attempt < 64; ++attempt) {
                const float ang = r.rangef(0.0f, 6.28318531f);
                const float rad = spread * std::sqrt(r.nextFloat());
                float cx = static_cast<float>(bestX) + std::cos(ang) * rad;
                float cy = static_cast<float>(bestY) + std::sin(ang) * rad;
                cx = std::min(std::max(cx, 0.0f), static_cast<float>(m_world.width()) - 1.0f);
                cy = std::min(std::max(cy, 0.0f), static_cast<float>(m_world.height()) - 1.0f);
                const size_t ti = m_world.index(static_cast<int>(cx), static_cast<int>(cy));
                if (m_world.elevation[ti] <= m_world.params().seaLevel) continue;
                if (m_world.waterDepth[ti] > 1.0f) continue;
                fx = cx;
                fy = cy;
                break;
            }
            // Alternate the heterogametic sex so the founding population is not
            // accidentally all one sex, which would end the run immediately.
            const bool hetero = (k % 2) == 1;
            if (m_agents.spawnFounder(fx, fy, m_rng, 0, hetero).valid()) ++placed;
        }
        m_agents.recomputeStats(0);
        m_agents.recomputePopulationGenetics();

        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "%d founders placed near (%d, %d)", placed, bestX, bestY);
        logEvent(EventKind::Info, msg, bestX, bestY);
    }

    {
        std::lock_guard<std::mutex> tl(m_telemetryMutex);
        m_series.clear();
    }
    {
        std::lock_guard<std::mutex> el(m_eventMutex);
        m_events.clear();
    }
    lk.unlock();

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "World created: %dx%d tiles, seed %llu, %.0f%% land, sea level %.0f m",
                  w, h, static_cast<unsigned long long>(seed),
                  m_world.stats().landFraction * 100.0, m_world.params().seaLevel);
    logEvent(EventKind::WorldCreated, buf);
    sampleTelemetry();
    publishSnapshot();
}

void Simulation::threadMain() {
    auto lastTime = Clock::now();
    const auto startTime = Clock::now();

    while (!m_quit.load(std::memory_order_relaxed)) {
        const double dt = secondsSince(lastTime);
        lastTime = Clock::now();
        m_realElapsed = secondsSince(startTime);

        drainCommands();

        uint64_t toRun = 0;
        const uint64_t steps = m_speed.takeStepRequest();
        if (steps > 0) {
            toRun = steps;
        } else {
            bool unlimited = false;
            const uint64_t owed = m_speed.ticksOwed(dt, unlimited);
            if (unlimited) {
                toRun = m_adaptiveBatch;
            } else {
                toRun = owed;
            }
        }

        if (toRun > 0) {
            const auto batchStart = Clock::now();
            runBatch(toRun);
            m_lastBatchMs = secondsSince(batchStart) * 1000.0;

            // Rolling throughput measurement over roughly a quarter second.
            m_windowTicks += static_cast<double>(toRun);
            m_windowSeconds += m_lastBatchMs * 0.001;
            if (m_windowSeconds > 0.25) {
                m_ticksPerSecond = m_windowTicks / m_windowSeconds;
                m_windowTicks = 0.0;
                m_windowSeconds = 0.0;
            }

            // Adapt the MAX-speed batch size towards the wall-clock budget, so
            // the world lock is held in predictable slices regardless of how
            // expensive a tick currently is.
            if (m_speed.maxSpeed()) {
                const double target = cfg().getFloat("sim.batch_target_ms", 2.0);
                if (m_lastBatchMs > 0.0001) {
                    const double scale = target / m_lastBatchMs;
                    double next = static_cast<double>(m_adaptiveBatch) *
                                  std::min(4.0, std::max(0.25, scale));
                    if (next < 1.0) next = 1.0;
                    if (next > 1000000.0) next = 1000000.0;
                    m_adaptiveBatch = static_cast<uint64_t>(next);
                }
            } else {
                m_adaptiveBatch = 1;
            }
            checkRunUntil();
        } else {
            // Nothing owed: yield rather than spin. 1 ms keeps the command
            // latency imperceptible while costing no measurable CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (m_ticksPerSecond > 0.0 && m_speed.paused()) m_ticksPerSecond = 0.0;
        }

        publishSnapshot();
    }
}

void Simulation::runBatch(uint64_t count) {
    std::unique_lock<std::shared_mutex> lk(m_worldMutex);
    for (uint64_t i = 0; i < count; ++i) {
        tickOnce();
        if (m_quit.load(std::memory_order_relaxed)) break;
    }
}

void Simulation::tickOnce() {
    // The canonical tick order from ARCHITECTURE.md §3. Steps 9 and 10
    // (chemistry, culture and disease) arrive with M5-M6; everything else is
    // here and runs on every agent on every tick at every speed.
    ++m_tick;

    m_profiler.beginTick();

    // Persistent disasters (ice age, volcanic winter, a spreading wildfire) are
    // resolved BEFORE the environment step so their global offsets apply to
    // this tick's climate rather than to the previous one.
    {
        ScopedStage g(m_profiler, Stage::Disasters);
        std::vector<std::string> disasterEvents;
        m_god.stepDisasters(m_world, m_agents, m_rng, m_tick, disasterEvents);
        for (const std::string& e : disasterEvents) logEvent(EventKind::Disaster, e);
        m_world.setClimateOverride(m_god.globalTemperatureOffset(),
                                   m_god.globalRainfallMultiplier());
    }

    m_world.step(m_tick, m_rng, m_jobs, m_profiler);   // 1-2: clock, environment

    if (m_agents.population() > 0) {
        { ScopedStage g(m_profiler, Stage::SpatialIndex);
          m_agents.buildSpatialIndex(m_world); }          // 3: neighbour index
        { ScopedStage g(m_profiler, Stage::Sense);
          m_agents.sense(m_world, m_tick, m_jobs); }      // 3: gather inputs
        { ScopedStage g(m_profiler, Stage::Think);
          m_agents.think(m_jobs); }                       // 4: evaluate every brain
        { ScopedStage g(m_profiler, Stage::Neighbours);
          m_agents.buildInteractionNeighbours(m_jobs, 1.5f); }  // 4.5: close-range index
        { ScopedStage g(m_profiler, Stage::Act);
          m_agents.act(m_world, m_rng, m_tick, m_jobs); }  // 5: apply motor outputs
        { ScopedStage g(m_profiler, Stage::Physics);
          m_agents.physics(m_world, m_jobs); }            // 6: integrate movement
        { ScopedStage g(m_profiler, Stage::Metabolism);
          m_agents.metabolism(m_world, m_rng, m_tick, m_jobs); }  // 7: energy, aging
        { ScopedStage g(m_profiler, Stage::Reproduction);
          m_agents.reproduction(m_world, m_rng, m_tick); }  // 8: courtship to birth
        { ScopedStage g(m_profiler, Stage::Chemistry);
          m_agents.chemistry(m_world, m_rng, m_tick); }    // 9: gather, experiment, teach
        // 10: the economy, IF one exists. The guard is here rather than inside
        // Economy::step so that with no economy nothing in econ/economy.cpp is
        // entered at all -- not a cheap early return, not entered. That is the
        // inertness contract in econ/economy.h, and this line is where it is
        // enforced.
        if (m_economy.active()) {
            ScopedStage g(m_profiler, Stage::Economy);
            std::vector<std::string> econEvents;
            m_economy.step(m_world, m_agents, m_rng, m_tick, econEvents);
            for (const std::string& e : econEvents) logEvent(EventKind::Emergence, e);
        }

        { ScopedStage g(m_profiler, Stage::Reap);
          m_agents.reapDead(m_world, m_tick); }            // 11: recycle slots

        // Drain whatever the population produced into the world event feed.
        for (Agents::PendingEvent& e : m_agents.pendingEvents())
            logEvent(static_cast<EventKind>(e.kind), e.text, e.x, e.y, e.subject);
        m_agents.pendingEvents().clear();

        if (m_agents.population() == 0 && !m_extinctionLogged) {
            m_extinctionLogged = true;
            logEvent(EventKind::Extinction,
                     "EXTINCTION: the last agent died. The world continues without them.");
        }
    }

    const uint64_t telemetryPeriod =
        static_cast<uint64_t>(cfg().getInt("sim.telemetry_period_ticks", 720));
    if (telemetryPeriod && m_tick % telemetryPeriod == 0) {
        ScopedStage g(m_profiler, Stage::Telemetry);
        m_world.recomputeStats(m_jobs);
        m_agents.recomputeStats(m_tick);
        sampleTelemetry();
    }

    const uint64_t speciesPeriod =
        static_cast<uint64_t>(cfg().getInt("species.period_ticks", 2160));
    if (speciesPeriod && m_tick % speciesPeriod == 0 && m_agents.population() > 0) {
        ScopedStage g(m_profiler, Stage::Species);
        m_agents.detectSpecies(m_tick);
        for (Agents::PendingEvent& e : m_agents.pendingEvents())
            logEvent(static_cast<EventKind>(e.kind), e.text, e.x, e.y, e.subject);
        m_agents.pendingEvents().clear();
    }

    if (m_economy.active()) {
        const uint64_t moneyPeriod =
            static_cast<uint64_t>(cfg().getInt("econ.detect_period_ticks", 2160));
        if (moneyPeriod && m_tick % moneyPeriod == 0) {
            ScopedStage g(m_profiler, Stage::Economy);
            std::vector<std::string> econEvents;
            m_economy.detectMoney(m_tick, econEvents);
            for (const std::string& e : econEvents) logEvent(EventKind::Emergence, e);
        }
    }

    const uint64_t statsPeriod = static_cast<uint64_t>(cfg().getInt("stats.period_ticks", 720));
    if (statsPeriod && m_tick % statsPeriod == 0 && m_agents.population() > 1) {
        ScopedStage g(m_profiler, Stage::PopGenetics);
        m_agents.recomputePopulationGenetics();
    }

    const uint64_t autosaveYears = static_cast<uint64_t>(cfg().getInt("sim.autosave_years", 25));
    if (autosaveYears > 0 && m_tick % kHoursPerYear == 0) {
        const uint64_t year = m_tick / kHoursPerYear;
        if (year >= m_lastAutosaveYear + autosaveYears) {
            m_lastAutosaveYear = year;
            char path[256];
            std::snprintf(path, sizeof(path), "autosave_y%llu.gen",
                          static_cast<unsigned long long>(year));
            // The world lock is already held by runBatch; saveSnapshotInternal
            // does not take it, by contract.
            if (saveSnapshotInternal(path)) {
                char msg[320];
                std::snprintf(msg, sizeof(msg), "Autosaved to %s", path);
                logEvent(EventKind::Info, msg);
            }
        }
    }
}

void Simulation::pushScript(const std::string& source) {
    std::lock_guard<std::mutex> lk(m_luaMutex);
    m_scriptQueue.push_back(source);
}

void Simulation::clearLuaOutput() {
    std::lock_guard<std::mutex> lk(m_luaMutex);
    m_lua.clearLines();
}

void Simulation::pushGodAction(const GodAction& a) {
    std::lock_guard<std::mutex> lk(m_godMutex);
    m_godQueue.push_back(a);
}

void Simulation::editGod(std::function<void(GodMode&)> fn, const std::string& description) {
    std::lock_guard<std::mutex> lk(m_godMutex);
    PendingGodEdit e;
    e.fn = std::move(fn);
    e.description = description;
    m_godEditQueue.push_back(std::move(e));
}

void Simulation::editAgents(std::function<void(Agents&, RngBank&)> fn,
                            const std::string& description) {
    std::lock_guard<std::mutex> lk(m_editMutex);
    PendingEdit e;
    e.fn = std::move(fn);
    e.description = description;
    m_editQueue.push_back(std::move(e));
}

void Simulation::drainCommands() {
    // Deferred agent edits first: they run on the sim thread with the world
    // lock held exclusively, at a tick boundary, so an edit can never tear
    // state or race a tick. Each one is logged as an intervention.
    {
        std::vector<PendingEdit> edits;
        {
            std::lock_guard<std::mutex> lk(m_editMutex);
            edits.swap(m_editQueue);
        }
        if (!edits.empty()) {
            std::unique_lock<std::shared_mutex> wlk(m_worldMutex);
            for (PendingEdit& e : edits) {
                e.fn(m_agents, m_rng);
                Command c;
                c.type = CommandType::EditAgent;
                c.text = e.description;
                c.appliedTick = m_tick;
                {
                    std::lock_guard<std::mutex> llk(m_logMutex);
                    m_interventionLog.push_back(c);
                }
            }
            wlk.unlock();
            for (PendingEdit& e : edits)
                logEvent(EventKind::Divine, e.description);
        }
    }

    // Lua chunks, run first so a script can set up state the queued actions
    // then act on.
    {
        std::vector<std::string> scripts;
        {
            std::lock_guard<std::mutex> lk(m_luaMutex);
            scripts.swap(m_scriptQueue);
        }
        if (!scripts.empty()) {
            std::vector<std::string> messages;
            {
                std::unique_lock<std::shared_mutex> wlk(m_worldMutex);
                std::lock_guard<std::mutex> llk(m_luaMutex);
                if (!m_lua.ready()) m_lua.init();
                LuaContext ctx;
                ctx.world = &m_world;
                ctx.agents = &m_agents;
                ctx.god = &m_god;
                ctx.rng = &m_rng;
                ctx.tick = m_tick;
                ctx.events = &messages;
                for (const std::string& src : scripts) m_lua.run(src, ctx);
                m_agents.recomputeStats(m_tick);
            }
            for (const std::string& src : scripts) {
                Command c;
                c.type = CommandType::RunScript;
                c.text = src;
                c.appliedTick = m_tick;
                std::lock_guard<std::mutex> llk2(m_logMutex);
                m_interventionLog.push_back(c);
            }
            for (const std::string& msg : messages) logEvent(EventKind::Divine, msg);
        }
    }

    // God actions, applied on the sim thread with the world lock held
    // exclusively so nothing can ever observe a half-finished intervention.
    {
        std::vector<GodAction>      actions;
        std::vector<PendingGodEdit> godEdits;
        {
            std::lock_guard<std::mutex> lk(m_godMutex);
            actions.swap(m_godQueue);
            godEdits.swap(m_godEditQueue);
        }
        if (!actions.empty() || !godEdits.empty()) {
            std::vector<std::string> messages;
            {
                std::unique_lock<std::shared_mutex> wlk(m_worldMutex);
                for (GodAction& a : actions)
                    messages.push_back(m_god.apply(a, m_world, m_agents,
                                                   m_rng, m_tick));
                for (PendingGodEdit& e : godEdits) {
                    e.fn(m_god);
                    messages.push_back(e.description);
                }
                m_agents.recomputeStats(m_tick);
            }
            for (const std::string& msg : messages) {
                logEvent(EventKind::Divine, msg);
                Command c;
                c.type = CommandType::GodAct;
                c.text = msg;
                c.appliedTick = m_tick;
                std::lock_guard<std::mutex> llk(m_logMutex);
                m_interventionLog.push_back(c);
            }
        }
    }

    std::vector<Command> batch;
    {
        std::lock_guard<std::mutex> lk(m_cmdMutex);
        if (m_cmdQueue.empty()) return;
        batch.swap(m_cmdQueue);
    }
    for (Command& c : batch) {
        c.appliedTick = m_tick;
        applyCommand(c);
        if (isIntervention(c.type)) {
            std::lock_guard<std::mutex> lk(m_logMutex);
            m_interventionLog.push_back(c);
        }
    }
}

void Simulation::applyCommand(Command& c) {
    switch (c.type) {
        case CommandType::SetPaused:
            m_speed.setPaused(c.a != 0.0);
            break;

        case CommandType::SetSpeed:
            m_speed.setSpeed(c.a);
            break;

        case CommandType::SetMaxSpeed:
            m_speed.setMax();
            break;

        case CommandType::StepTicks:
            m_speed.requestSteps(static_cast<uint64_t>(c.a));
            m_speed.setPaused(true);
            break;

        case CommandType::SetRunUntil: {
            m_runUntil.kind = static_cast<RunUntilKind>(c.ix);
            m_runUntil.value = c.a;
            m_runUntil.text = c.text;
            m_runUntil.startTick = m_tick;
            m_runUntil.active = true;
            m_speed.setMax();
            break;
        }

        case CommandType::CancelRunUntil:
            m_runUntil.clear();
            m_speed.setPaused(true);
            break;

        case CommandType::RegenerateWorld: {
            const uint64_t seed = (c.a < 0.0) ? m_rng.worldSeed()
                                              : static_cast<uint64_t>(c.a);
            const int w = (c.ix > 0) ? c.ix : m_world.width();
            const int h = (c.iy > 0) ? c.iy : m_world.height();
            m_speed.setPaused(true);
            regenerate(seed, w, h);
            break;
        }

        case CommandType::SetConfigValue:
            cfg().setFloat(c.text.c_str(), c.a);
            break;

        case CommandType::SaveSnapshot: {
            std::unique_lock<std::shared_mutex> lk(m_worldMutex);
            const bool ok = saveSnapshotInternal(c.text);
            lk.unlock();
            logEvent(EventKind::Info, ok ? ("Saved snapshot to " + c.text)
                                         : ("FAILED to save snapshot to " + c.text));
            break;
        }

        case CommandType::LoadSnapshot: {
            m_speed.setPaused(true);
            const bool ok = loadSnapshotInternal(c.text);
            logEvent(EventKind::Info, ok ? ("Loaded snapshot " + c.text)
                                         : ("FAILED to load snapshot " + c.text));
            break;
        }

        case CommandType::AddBookmark: {
            Bookmark bm;
            bm.name = c.text.empty() ? tickToDate(m_tick).toShortString() : c.text;
            bm.tick = m_tick;
            char path[256];
            std::snprintf(path, sizeof(path), "bookmark_%llu.gen",
                          static_cast<unsigned long long>(m_tick));
            bm.filePath = path;
            {
                std::unique_lock<std::shared_mutex> lk(m_worldMutex);
                saveSnapshotInternal(bm.filePath);
            }
            {
                std::lock_guard<std::mutex> lk(m_logMutex);
                m_bookmarks.push_back(bm);
            }
            logEvent(EventKind::Info, "Bookmark: " + bm.name);
            break;
        }

        case CommandType::JumpToBookmark: {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(m_logMutex);
                const size_t i = static_cast<size_t>(c.ix);
                if (i < m_bookmarks.size()) path = m_bookmarks[i].filePath;
            }
            if (!path.empty()) {
                m_speed.setPaused(true);
                loadSnapshotInternal(path);
                logEvent(EventKind::Divine, "Jumped back to bookmark " + path);
            }
            break;
        }

        case CommandType::ExportTelemetry:
            exportTelemetryCsv(c.text);
            break;

        case CommandType::SpawnAgents: {
            std::unique_lock<std::shared_mutex> lk(m_worldMutex);
            int placed = 0;
            for (int i = 0; i < c.ix; ++i) {
                const float ang = m_rng[Stream::God].rangef(0.0f, 6.28318531f);
                const float rad = static_cast<float>(c.b) * std::sqrt(m_rng[Stream::God].nextFloat());
                const float x = static_cast<float>(c.a) + std::cos(ang) * rad;
                const float y = static_cast<float>(c.iy) + std::sin(ang) * rad;
                if (m_agents.spawnFounder(x, y, m_rng, m_tick, (i % 2) == 1).valid()) ++placed;
            }
            m_agents.recomputeStats(m_tick);
            lk.unlock();
            char msg[160];
            std::snprintf(msg, sizeof(msg), "Divine creation: %d agents spawned", placed);
            logEvent(EventKind::Divine, msg, static_cast<int32_t>(c.a), c.iy);
            break;
        }

        case CommandType::EditAgent:
            // Applied through the deferred edit queue in drainCommands; this
            // arm exists only so the log entry has a name.
            break;

        case CommandType::Undo:
        case CommandType::Redo: {
            std::string what;
            bool ok = false;
            {
                std::unique_lock<std::shared_mutex> lk(m_worldMutex);
                ok = (c.type == CommandType::Undo)
                   ? m_god.undo(m_world, m_agents, m_tick, what)
                   : m_god.redo(m_world, m_agents, m_tick, what);
                if (ok) m_agents.recomputeStats(m_tick);
            }
            logEvent(EventKind::Divine, ok ? what
                     : (c.type == CommandType::Undo ? "Nothing left to undo"
                                                    : "Nothing left to redo"));
            break;
        }

        case CommandType::CastMiracle: {
            std::vector<std::string> messages;
            {
                std::unique_lock<std::shared_mutex> lk(m_worldMutex);
                const size_t i = static_cast<size_t>(c.ix);
                if (i < m_god.miracles().size()) {
                    // A miracle is a recorded sequence replayed in order. Each
                    // step lands on the undo stack in its own right, so a
                    // miracle can be unwound one act at a time.
                    Miracle m = m_god.miracles()[i];
                    for (GodAction a : m.actions)
                        messages.push_back(m_god.apply(a, m_world, m_agents, m_rng, m_tick));
                    m_agents.recomputeStats(m_tick);
                    messages.push_back("Miracle complete: " + m.name);
                }
            }
            for (const std::string& msg : messages) logEvent(EventKind::Divine, msg);
            break;
        }

        case CommandType::GodAct:
        case CommandType::RunScript:
            // Both are applied earlier in drainCommands, under the world lock,
            // because they mutate simulation state rather than pacing. These
            // arms exist so the switch stays exhaustive.
            break;

        case CommandType::None:
        case CommandType::Count:
            break;
    }
}

void Simulation::checkRunUntil() {
    if (!m_runUntil.active) return;
    bool done = false;

    switch (m_runUntil.kind) {
        case RunUntilKind::Year:
            done = (static_cast<double>(m_tick) / static_cast<double>(kHoursPerYear))
                   >= m_runUntil.value;
            break;
        case RunUntilKind::ElapsedYears:
            done = (static_cast<double>(m_tick - m_runUntil.startTick) /
                    static_cast<double>(kHoursPerYear)) >= m_runUntil.value;
            break;
        case RunUntilKind::PopulationBelow:
            done = static_cast<double>(m_agents.population()) < m_runUntil.value;
            break;
        case RunUntilKind::PopulationAbove:
            done = static_cast<double>(m_agents.population()) > m_runUntil.value;
            break;
        case RunUntilKind::Extinction:
            done = m_agents.population() == 0;
            break;
        // Discovery and Lua predicates land with the milestones that create the
        // state they test. Until then they never trip, which is why the UI
        // marks them unavailable rather than offering a dead control.
        case RunUntilKind::FirstDiscovery:
        case RunUntilKind::Custom:
        case RunUntilKind::None:
            break;
    }

    if (done) {
        m_runUntil.active = false;
        m_speed.setPaused(true);
        logEvent(EventKind::Info, "Run-until condition met: " + m_runUntil.text);
    }
}

void Simulation::logEvent(EventKind kind, const std::string& text, int32_t x, int32_t y,
                          uint64_t subject) {
    WorldEvent e;
    e.tick = m_tick;
    e.kind = kind;
    e.text = text;
    e.x = x;
    e.y = y;
    e.subject = subject;
    // In a headless run the milestone events are the whole point of the run, so
    // they go to stdout as they happen rather than only into a log nobody sees.
    // Routine births and deaths do not: they would bury everything else.
    if (m_headlessVerbose) {
        switch (kind) {
            case EventKind::Emergence:
            case EventKind::Extinction:
            case EventKind::Fixation:
            case EventKind::Discovery:
                std::printf("  [yr %7.2f] %s\n",
                            static_cast<double>(m_tick) / static_cast<double>(kHoursPerYear),
                            text.c_str());
                std::fflush(stdout);
                break;
            default:
                break;
        }
    }

    const size_t cap = static_cast<size_t>(cfg().getInt("sim.event_log_capacity", 20000));
    std::lock_guard<std::mutex> lk(m_eventMutex);
    m_events.push_back(std::move(e));
    while (m_events.size() > cap) m_events.pop_front();
}

void Simulation::addSeriesPoint(const char* key, const char* label, const char* unit, float v) {
    // Caller holds m_telemetryMutex.
    for (Series& s : m_series) {
        if (s.key == key) {
            s.ticks.push_back(m_tick);
            s.values.push_back(v);
            const size_t cap = static_cast<size_t>(cfg().getInt("sim.telemetry_capacity", 60000));
            if (s.values.size() > cap) {
                // Drop the oldest half in one go rather than one per sample,
                // which would make every sample an O(n) erase.
                const size_t drop = s.values.size() / 2;
                s.ticks.erase(s.ticks.begin(), s.ticks.begin() + static_cast<long>(drop));
                s.values.erase(s.values.begin(), s.values.begin() + static_cast<long>(drop));
            }
            return;
        }
    }
    Series s;
    s.key = key;
    s.label = label;
    s.unit = unit;
    s.ticks.push_back(m_tick);
    s.values.push_back(v);
    m_series.push_back(std::move(s));
}

void Simulation::sampleTelemetry() {
    const WorldStats& w = m_world.stats();
    std::lock_guard<std::mutex> lk(m_telemetryMutex);
    addSeriesPoint("world.mean_temperature", "Mean temperature", "C",
                   static_cast<float>(w.meanTemperature));
    addSeriesPoint("world.mean_rainfall", "Mean rainfall", "mm/yr",
                   static_cast<float>(w.meanRainfall));
    addSeriesPoint("world.total_biomass", "Total plant biomass", "kg",
                   static_cast<float>(w.totalBiomass));
    addSeriesPoint("world.land_fraction", "Land fraction", "",
                   static_cast<float>(w.landFraction));
    addSeriesPoint("world.ice_fraction", "Ice cover fraction", "",
                   static_cast<float>(w.iceFraction));
    addSeriesPoint("world.soil_fertility", "Mean soil fertility", "",
                   static_cast<float>(w.meanSoilFertility));

    const PopulationStats& p = m_agents.stats();
    addSeriesPoint("pop.population", "Population", "agents", static_cast<float>(p.population));
    addSeriesPoint("pop.births", "Births (cumulative)", "", static_cast<float>(p.births));
    addSeriesPoint("pop.deaths", "Deaths (cumulative)", "", static_cast<float>(p.deaths));
    addSeriesPoint("pop.juveniles", "Juveniles", "",
                   static_cast<float>(p.byStage[static_cast<int>(LifeStage::Juvenile)]));
    addSeriesPoint("pop.adults", "Adults", "",
                   static_cast<float>(p.byStage[static_cast<int>(LifeStage::Adult)]));
    addSeriesPoint("pop.pregnancies", "Pregnancies", "", static_cast<float>(p.pregnancies));
    addSeriesPoint("pop.mean_age", "Mean age", "yr", static_cast<float>(p.meanAgeYears));
    addSeriesPoint("pop.mean_lifespan", "Mean lifespan at death", "yr",
                   static_cast<float>(p.meanLifespanAtDeath));
    addSeriesPoint("pop.mean_health", "Mean health", "", static_cast<float>(p.meanHealth));

    // Brain complexity over time. Rising mean node and connection counts are
    // the visible signature of gene duplication and add-node mutation paying off.
    addSeriesPoint("brain.mean_nodes", "Mean brain neurons", "", static_cast<float>(p.meanBrainNodes));
    addSeriesPoint("brain.mean_connections", "Mean brain synapses", "",
                   static_cast<float>(p.meanBrainConns));
    addSeriesPoint("gene.mean_genome_length", "Mean genome length", "genes",
                   static_cast<float>(p.meanGenomeLength));

    // Sexual selection. Watching mean ornament and mean ornament PREFERENCE
    // climb together is what Fisherian runaway looks like from the outside.
    addSeriesPoint("sex.mean_expression", "Mean sex expression", "",
                   static_cast<float>(p.meanSexExpression));
    addSeriesPoint("sex.intersex_fraction", "Intersex fraction", "",
                   static_cast<float>(p.intersexFraction));
    addSeriesPoint("sex.mean_ornament", "Mean ornament", "", static_cast<float>(p.meanOrnament));
    addSeriesPoint("sex.mean_ornament_preference", "Mean ornament preference", "",
                   static_cast<float>(p.meanPrefOrnament));
    addSeriesPoint("sex.bonded_fraction", "Pair-bonded fraction", "",
                   static_cast<float>(p.bondedFraction));
    addSeriesPoint("sex.mean_repro_reward", "Mean reproduction reward weight", "",
                   static_cast<float>(p.meanRewardRepro));

    // Culture. Plotted alongside genetics rather than merged into it, because
    // it moves on a different timescale and by a different mechanism -- and
    // because knowledge units can FALL, which no allele frequency does for the
    // same reason.
    addSeriesPoint("culture.discoveries", "Things ever discovered", "",
                   static_cast<float>(p.discoveries));
    addSeriesPoint("culture.knowledge_units", "Knowledge units held", "",
                   static_cast<float>(p.knowledgeUnits));
    addSeriesPoint("culture.mean_knowledge", "Knowledge units per individual", "",
                   static_cast<float>(p.meanKnowledge));
    addSeriesPoint("culture.technology_index", "Technology index", "",
                   static_cast<float>(p.technologyIndex));
    addSeriesPoint("culture.fire", "Individuals who can make fire", "",
                   static_cast<float>(p.agentsWithFire));
    addSeriesPoint("culture.kiln", "Individuals who can fire a kiln", "",
                   static_cast<float>(p.agentsWithKiln));
    addSeriesPoint("culture.bellows", "Individuals with a bellows furnace", "",
                   static_cast<float>(p.agentsWithBellows));

    const Genetics::PopulationGenetics& g = m_agents.populationGenetics();
    addSeriesPoint("gene.heterozygosity_obs", "Observed heterozygosity", "",
                   static_cast<float>(g.observedHeterozygosity));
    addSeriesPoint("gene.heterozygosity_exp", "Expected heterozygosity", "",
                   static_cast<float>(g.expectedHeterozygosity));
    addSeriesPoint("gene.inbreeding_f", "Mean inbreeding coefficient F", "",
                   static_cast<float>(g.meanInbreedingF));
    addSeriesPoint("gene.tajimas_d", "Tajima's D", "", static_cast<float>(g.tajimasD));
    addSeriesPoint("gene.effective_pop_size", "Effective population size Ne", "",
                   static_cast<float>(g.effectivePopulationSize));
    addSeriesPoint("gene.segregating_sites", "Segregating loci", "",
                   static_cast<float>(g.segregatingSites));
    addSeriesPoint("gene.fixed_loci", "Fixed loci", "", static_cast<float>(g.fixedLoci));
    addSeriesPoint("gene.linkage_disequilibrium", "Linkage disequilibrium (mean r2)", "",
                   static_cast<float>(g.linkageDisequilibrium));
    addSeriesPoint("gene.fst", "Fst between regions", "", static_cast<float>(m_agents.fst()));
}

void Simulation::publishSnapshot() {
    SimSnapshot s;
    s.tick = m_tick;
    s.date = tickToDate(m_tick);
    s.season = seasonOfTick(m_tick);
    s.simYears = static_cast<double>(m_tick) / static_cast<double>(kHoursPerYear);
    s.ticksPerSecond = m_ticksPerSecond;
    // Effective multiplier IS ticks/sec, because 1x is defined as one simulated
    // hour per real second. Reporting it twice under two names is deliberate:
    // one is the raw throughput, the other is what the speed buttons mean.
    s.effectiveMultiplier = m_speed.paused() ? 0.0 : m_ticksPerSecond;
    s.requestedSpeed = m_speed.requested();
    s.paused = m_speed.paused();
    s.maxSpeed = m_speed.maxSpeed();
    // The bottleneck is now MEASURED rather than guessed. The old value was
    // whichever stage happened to run last, which named the environment on
    // every sixth tick regardless of what the tick actually cost.
    s.bottleneck = stageName(m_profiler.dominant());
    for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
        const Stage st = static_cast<Stage>(i);
        s.stageMeanMs[i] = m_profiler.meanMs(st);
        s.stagePeakMs[i] = m_profiler.peakMs(st);
        s.stageTotalMs[i] = m_profiler.totalMs(st);
    }
    s.economyActive = m_economy.active();
    s.hasCurrency = m_economy.hasCurrency();
    s.currencyName = m_economy.currencyName();
    s.totalTrades = m_economy.totalExchanges();
    s.coincidenceRate = m_economy.coincidenceRate();
    s.gini = m_economy.giniCoefficient(m_agents);
    s.tickMeanMs = m_profiler.meanTickMs();
    s.profiledTicks = m_profiler.ticks();
    s.lastBatchMs = m_lastBatchMs;
    s.batchSize = m_adaptiveBatch;
    s.realElapsedSeconds = m_realElapsed;
    s.world = m_world.stats();
    s.population = m_agents.stats();
    s.popGenetics = m_agents.populationGenetics();
    s.fst = m_agents.fst();
    s.agentCount = m_agents.population();
    s.seed = m_rng.worldSeed();
    s.workerCount = m_jobs.workerCount();

    s.undoDepth = static_cast<uint32_t>(m_god.undoDepth());
    s.redoDepth = static_cast<uint32_t>(m_god.redoDepth());
    s.undoMemoryMb = static_cast<double>(m_god.undoMemoryBytes()) / (1024.0 * 1024.0);
    s.activeDisasters = static_cast<uint32_t>(m_god.disasters().size());
    s.temperatureOffset = m_god.globalTemperatureOffset();
    s.rainfallMultiplier = m_god.globalRainfallMultiplier();

    s.runUntilActive = m_runUntil.active;
    s.runUntilLabel = m_runUntil.text;
    if (m_runUntil.active) {
        double progress = 0.0;
        if (m_runUntil.kind == RunUntilKind::Year && m_runUntil.value > 0.0) {
            progress = (static_cast<double>(m_tick) / static_cast<double>(kHoursPerYear))
                       / m_runUntil.value;
        } else if (m_runUntil.kind == RunUntilKind::ElapsedYears && m_runUntil.value > 0.0) {
            progress = (static_cast<double>(m_tick - m_runUntil.startTick) /
                        static_cast<double>(kHoursPerYear)) / m_runUntil.value;
        }
        s.runUntilProgress = (progress < 0.0) ? 0.0 : (progress > 1.0 ? 1.0 : progress);
    }

    {
        std::lock_guard<std::mutex> lk(g_genProgress.mutex);
        s.genProgress = g_genProgress.value;
        s.genStage = g_genProgress.stage;
    }
    s.worldReady = m_world.valid() && s.genProgress >= 1.0f;

    {
        std::lock_guard<std::mutex> lk(m_logMutex);
        s.interventionCount = m_interventionLog.size();
    }
    {
        std::lock_guard<std::mutex> lk(m_snapshotMutex);
        m_snapshot = std::move(s);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool Simulation::saveSnapshotInternal(const std::string& path) {
    BinaryWriter w(path);
    if (!w.ok()) {
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = "cannot open " + path + " for writing";
        return false;
    }

    SnapshotHeader h;
    h.compilerTag = currentCompilerTag();
    h.worldSeed = m_rng.worldSeed();
    h.workerCount = m_jobs.workerCount();
    h.tick = m_tick;
    w.writeHeader(h);

    m_world.serialize(w);

    // RNG stream states, so a loaded world continues the same number sequence
    // rather than restarting it.
    w.beginChunk(kChunkRng);
    const uint32_t streamCount = RngBank::kCount;
    w.pod(streamCount);
    for (uint32_t i = 0; i < streamCount; ++i) {
        Rng& r = m_rng[static_cast<Stream>(i)];
        const uint64_t* st = r.state();
        for (int k = 0; k < 4; ++k) w.pod(st[k]);
        const uint8_t hasSpare = r.hasSpare() ? 1u : 0u;
        w.pod(hasSpare);
        const float spare = r.spare();
        w.pod(spare);
    }
    w.endChunk();

    w.beginChunk(kChunkTime);
    w.pod(m_tick);
    w.pod(m_lastAutosaveYear);
    w.endChunk();

    w.beginChunk(kChunkEcon);
    m_economy.serialize(w);
    w.endChunk();

    w.beginChunk(kChunkGod);
    m_god.serialize(w);
    w.endChunk();

    // One chunk carries the whole population: bodies, genomes, brains and
    // pedigree. A world with no agents writes it with a zero count rather than
    // omitting it, so the reader never has to guess.
    w.beginChunk(kChunkAgents);
    m_agents.serialize(w);
    w.endChunk();

    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        w.beginChunk(kChunkEvents);
        const uint32_t n = static_cast<uint32_t>(m_events.size());
        w.pod(n);
        for (const WorldEvent& e : m_events) {
            w.pod(e.tick);
            const uint8_t k = static_cast<uint8_t>(e.kind);
            w.pod(k);
            w.pod(e.x);
            w.pod(e.y);
            w.pod(e.subject);
            w.str(e.text);
        }
        w.endChunk();
    }

    {
        std::lock_guard<std::mutex> lk(m_telemetryMutex);
        w.beginChunk(kChunkTelem);
        const uint32_t n = static_cast<uint32_t>(m_series.size());
        w.pod(n);
        for (const Series& s : m_series) {
            w.str(s.key);
            w.str(s.label);
            w.str(s.unit);
            w.array(s.ticks);
            w.array(s.values);
        }
        w.endChunk();
    }

    {
        std::lock_guard<std::mutex> lk(m_logMutex);
        w.beginChunk(kChunkIntervn);
        const uint32_t n = static_cast<uint32_t>(m_interventionLog.size());
        w.pod(n);
        for (const Command& c : m_interventionLog) {
            const uint16_t t = static_cast<uint16_t>(c.type);
            w.pod(t);
            w.pod(c.a);
            w.pod(c.b);
            w.pod(c.ix);
            w.pod(c.iy);
            w.pod(c.appliedTick);
            w.str(c.text);
        }
        w.endChunk();
    }

    w.close();
    return true;
}

bool Simulation::loadSnapshotInternal(const std::string& path) {
    BinaryReader r(path);
    if (!r.ok()) {
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = r.error();
        return false;
    }

    SnapshotHeader h;
    if (!r.readHeader(h)) {
        std::lock_guard<std::mutex> lk(m_errorMutex);
        m_lastError = r.error();
        return false;
    }

    std::unique_lock<std::shared_mutex> lk(m_worldMutex);

    m_rng.reseed(h.worldSeed);
    m_tick = h.tick;

    // Worker count is restored from the file: chunk partitioning affects float
    // summation order, so reproducing a run bit-for-bit requires the same split.
    if (h.workerCount != m_jobs.workerCount()) {
        m_jobs.shutdown();
        m_jobs.start(h.workerCount);
    }

    std::vector<WorldEvent> events;
    std::vector<Series>     series;
    std::vector<Command>    interventions;

    struct SavedRng { uint64_t s[4] = {0, 0, 0, 0}; uint8_t hasSpare = 0; float spare = 0.0f; };
    std::vector<SavedRng> rngStates;
    bool haveRngStates = false;

    uint32_t tag = 0;
    uint64_t length = 0;
    while (r.nextChunk(tag, length)) {
        switch (tag) {
            case kChunkWorld:
            case kChunkTiles:
                m_world.deserialize(r, tag);
                break;

            case kChunkRng: {
                uint32_t n = 0;
                r.pod(n);
                // Buffered, not applied yet. Loading the agent chunk calls
                // Agents::configure(), which rebuilds the recombination map and
                // gene template and so CONSUMES from the Genetics stream. If
                // the states were applied here, that consumption would leave
                // every stream slightly ahead of where it was at save time and
                // the resumed run would diverge from the continuous one.
                rngStates.resize(n);
                for (uint32_t i = 0; i < n; ++i) {
                    for (int k = 0; k < 4; ++k) r.pod(rngStates[i].s[k]);
                    r.pod(rngStates[i].hasSpare);
                    r.pod(rngStates[i].spare);
                }
                haveRngStates = true;
                break;
            }

            case kChunkTime:
                r.pod(m_tick);
                r.pod(m_lastAutosaveYear);
                break;

            case kChunkEcon:
                m_economy.deserialize(r);
                break;
            case kChunkGod:
                m_god.deserialize(r);
                break;

            case kChunkAgents:
                // The store must be sized before it is filled, and its sizing
                // comes from config rather than from the file, so a snapshot
                // can be loaded into a build with a different max_agents as
                // long as the saved population fits.
                m_agents.configure(static_cast<size_t>(cfg().getInt("sim.max_agents", 12000)),
                                   m_rng[Stream::Genetics]);
                m_agents.deserialize(r);
                break;

            case kChunkEvents: {
                uint32_t n = 0;
                r.pod(n);
                if (n > 2000000u) { r.skipChunk(); break; }
                for (uint32_t i = 0; i < n && r.ok(); ++i) {
                    WorldEvent e;
                    r.pod(e.tick);
                    uint8_t k = 0;
                    r.pod(k);
                    e.kind = static_cast<EventKind>(k);
                    r.pod(e.x);
                    r.pod(e.y);
                    r.pod(e.subject);
                    r.str(e.text);
                    events.push_back(std::move(e));
                }
                break;
            }

            case kChunkTelem: {
                uint32_t n = 0;
                r.pod(n);
                if (n > 100000u) { r.skipChunk(); break; }
                for (uint32_t i = 0; i < n && r.ok(); ++i) {
                    Series s;
                    r.str(s.key);
                    r.str(s.label);
                    r.str(s.unit);
                    r.array(s.ticks);
                    r.array(s.values);
                    series.push_back(std::move(s));
                }
                break;
            }

            case kChunkIntervn: {
                uint32_t n = 0;
                r.pod(n);
                if (n > 5000000u) { r.skipChunk(); break; }
                for (uint32_t i = 0; i < n && r.ok(); ++i) {
                    Command c;
                    uint16_t t = 0;
                    r.pod(t);
                    c.type = static_cast<CommandType>(t);
                    r.pod(c.a);
                    r.pod(c.b);
                    r.pod(c.ix);
                    r.pod(c.iy);
                    r.pod(c.appliedTick);
                    r.str(c.text);
                    interventions.push_back(std::move(c));
                }
                break;
            }

            default:
                // Unknown chunk from a newer build: skip by length. This is
                // what makes the format forward and backward compatible.
                r.skipChunk();
                break;
        }
    }
    // Applied LAST, after every chunk has been read, so that whatever the
    // subsystem constructors consumed along the way is undone and the streams
    // resume exactly where they stood when the snapshot was written.
    if (haveRngStates) {
        for (uint32_t i = 0; i < rngStates.size() && i < RngBank::kCount; ++i)
            m_rng[static_cast<Stream>(i)].setState(rngStates[i].s,
                                                   rngStates[i].hasSpare != 0,
                                                   rngStates[i].spare);
    }

    lk.unlock();

    if (!r.ok()) {
        std::lock_guard<std::mutex> el(m_errorMutex);
        m_lastError = r.error();
        return false;
    }

    {
        std::lock_guard<std::mutex> el(m_eventMutex);
        m_events.assign(events.begin(), events.end());
    }
    {
        std::lock_guard<std::mutex> tl(m_telemetryMutex);
        m_series = std::move(series);
    }
    {
        std::lock_guard<std::mutex> il(m_logMutex);
        m_interventionLog = std::move(interventions);
    }

    {
        std::lock_guard<std::mutex> gl(g_genProgress.mutex);
        g_genProgress.value = 1.0f;
        g_genProgress.stage = "Loaded";
    }
    publishSnapshot();

    if (h.compilerTag != currentCompilerTag()) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Snapshot was written by %s; this build is %s. State is restored exactly, "
                      "but bit-identical continuation is not guaranteed across libm versions.",
                      compilerTagName(h.compilerTag), compilerTagName(currentCompilerTag()));
        logEvent(EventKind::Info, msg);
    }
    return true;
}

bool Simulation::saveSnapshotNow(const std::string& path) {
    std::unique_lock<std::shared_mutex> lk(m_worldMutex);
    return saveSnapshotInternal(path);
}

bool Simulation::loadSnapshotNow(const std::string& path) {
    return loadSnapshotInternal(path);
}

bool Simulation::exportTelemetryCsv(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_telemetryMutex);
    if (m_series.empty()) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    // Series are sampled together on the same cadence, so a single tick column
    // is valid. The longest series defines the row count; shorter ones are
    // left blank rather than padded with a misleading zero.
    size_t rows = 0;
    for (const Series& s : m_series) rows = std::max(rows, s.values.size());

    std::fprintf(f, "tick,year,month,day,hour");
    for (const Series& s : m_series) {
        std::fprintf(f, ",%s", s.key.c_str());
        if (!s.unit.empty()) std::fprintf(f, " (%s)", s.unit.c_str());
    }
    std::fprintf(f, "\n");

    for (size_t row = 0; row < rows; ++row) {
        uint64_t tick = 0;
        for (const Series& s : m_series) {
            if (row < s.ticks.size()) { tick = s.ticks[row]; break; }
        }
        const DateTime d = tickToDate(tick);
        std::fprintf(f, "%llu,%lld,%d,%d,%d", static_cast<unsigned long long>(tick),
                     static_cast<long long>(d.year), d.month + 1, d.day + 1, d.hour);
        for (const Series& s : m_series) {
            if (row < s.values.size()) std::fprintf(f, ",%.9g", static_cast<double>(s.values[row]));
            else std::fprintf(f, ",");
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Headless batch mode
// ---------------------------------------------------------------------------

bool Simulation::runHeadless(const WorldParams& params, double years,
                             const std::string& snapshotPath, const std::string& csvPath,
                             bool verbose, const std::string& loadPath) {
    m_god.setEconomy(&m_economy);
    m_headlessVerbose = verbose;

    unsigned workers = static_cast<unsigned>(cfg().getInt("sim.worker_threads", 0));
    if (workers == 0) workers = JobSystem::recommendedWorkers();
    m_jobs.start(workers);

    const auto t0 = Clock::now();

    if (!loadPath.empty()) {
        if (verbose) {
            std::printf("GENESIS headless: resuming from %s\n", loadPath.c_str());
            std::fflush(stdout);
        }
        if (!loadSnapshotNow(loadPath)) {
            std::printf("Failed to load %s: %s\n", loadPath.c_str(), lastError().c_str());
            std::fflush(stdout);
            m_jobs.shutdown();
            return false;
        }
        if (verbose) {
            std::printf("Resumed at %s (%dx%d, seed %llu, %u worker threads)\n",
                        tickToDate(m_tick).toString().c_str(),
                        m_world.width(), m_world.height(),
                        static_cast<unsigned long long>(m_rng.worldSeed()), m_jobs.workerCount());
            std::fflush(stdout);
        }
    } else {
        if (verbose) {
            std::printf("GENESIS headless: %dx%d, seed %llu, %u worker threads\n",
                        params.width, params.height,
                        static_cast<unsigned long long>(params.seed), workers);
            std::fflush(stdout);
        }
        regenerate(params.seed, params.width, params.height);
        if (verbose) {
            std::printf("World generated in %.2f s (%.1f%% land, sea level %.0f m)\n",
                        secondsSince(t0), m_world.stats().landFraction * 100.0,
                        m_world.params().seaLevel);
            std::fflush(stdout);
        }
    }

    if (!m_startupScript.empty()) {
        m_god.configure();
        if (!m_lua.ready()) m_lua.init();
        std::vector<std::string> messages;
        LuaContext ctx;
        ctx.world = &m_world;
        ctx.agents = &m_agents;
        ctx.god = &m_god;
        ctx.rng = &m_rng;
        ctx.tick = m_tick;
        ctx.events = &messages;
        m_lua.run(m_startupScript, ctx);
        m_agents.recomputeStats(m_tick);
        if (verbose) {
            for (const LuaLine& l : m_lua.lines())
                std::printf("  [lua] %s\n", l.text.c_str());
            for (const std::string& msg : messages) std::printf("  [god] %s\n", msg.c_str());
            std::fflush(stdout);
        }
    }

    const uint64_t totalTicks = static_cast<uint64_t>(years * static_cast<double>(kHoursPerYear));
    const uint64_t reportEvery = kHoursPerYear;  // once per simulated year
    const auto tSim = Clock::now();

    for (uint64_t i = 0; i < totalTicks; ++i) {
        tickOnce();

        // Population diagnostic: what the whole cohort is doing, which is what
        // actually explains a die-off. A single traced individual only ever
        // shows you a survivor or a corpse, never the distribution.
        if (m_traceAgentPeriod > 0 && m_tick % m_traceAgentPeriod == 0 &&
            m_agents.population() > 0) {
            int onBare = 0, hungry = 0, eating = 0, n = 0;
            double energySum = 0.0, eatOutSum = 0.0, bioSum = 0.0;
            for (uint32_t s : m_agents.liveSlots()) {
                if (m_agents.m_stage[s] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
                ++n;
                energySum += m_agents.m_energy[s];
                eatOutSum += m_agents.m_outputs[s * kBrainOutputCount + Out_Eat];
                if (m_agents.m_drives[s][Drive::Hunger] > 0.6f) ++hungry;
                if (m_agents.m_action[s] == static_cast<uint8_t>(Action::Eat)) ++eating;
                const int ax = static_cast<int>(m_agents.m_x[s]);
                const int ay = static_cast<int>(m_agents.m_y[s]);
                if (m_world.inBounds(ax, ay)) {
                    const float b = m_world.biomass[m_world.index(ax, ay)];
                    bioSum += b;
                    if (b <= 0.5f) ++onBare;
                }
            }
            if (n > 0)
                std::printf("  [pop] t%-7llu n=%-4d meanE=%6.1f hungry=%-4d eating=%-4d "
                            "onBareTile=%-4d meanTileBio=%7.1f meanEatOut=%+.2f\n",
                            static_cast<unsigned long long>(m_tick), n, energySum / n,
                            hungry, eating, onBare, bioSum / n, eatOutSum / n);
            std::fflush(stdout);
        }

        // Agent trace: dump one individual's inner state on a fixed cadence.
        // Invaluable when a population dies and the aggregate numbers do not
        // say why -- which aggregate numbers rarely do.
        // Follows a FIXED individual by uid, not "whoever is in slot 0" -- slot
        // recycling would otherwise silently switch subjects mid-trace and make
        // a stationary agent look like a moving one.
        if (m_traceAgentPeriod > 0 && m_tick % m_traceAgentPeriod == 0 &&
            m_agents.slotOfUid(m_traceAgentUid) >= 0) {
            const uint32_t slot = static_cast<uint32_t>(m_agents.slotOfUid(m_traceAgentUid));
            const Phenotype& ph = m_agents.m_phenotype[slot];
            const int tx = static_cast<int>(m_agents.m_x[slot]);
            const int ty = static_cast<int>(m_agents.m_y[slot]);
            const float bio = m_world.inBounds(tx, ty)
                ? m_world.biomass[m_world.index(tx, ty)] : -1.0f;
            const float temp = m_world.inBounds(tx, ty)
                ? m_world.temperature[m_world.index(tx, ty)] : 0.0f;
            const float wd = m_world.inBounds(tx, ty)
                ? m_world.waterDepth[m_world.index(tx, ty)] : -1.0f;
            if (m_world.inBounds(tx, ty)) {
                const size_t ti = m_world.index(tx, ty);
                const float optT = cfg().getF("ecology.optimal_temperature", 22.0f);
                const float tolT = cfg().getF("ecology.temperature_tolerance", 16.0f);
                const float dt = (m_world.temperature[ti] - optT) / tolT;
                const float nutrient = static_cast<float>(std::min(
                    std::min(m_world.soilN[ti], m_world.soilP[ti]), m_world.soilK[ti])) / 255.0f;
                std::printf("      [tile %d,%d] fT=%.3f fW=%.3f nutrient=%.3f (N%u P%u K%u) rain=%.0f\n",
                            tx, ty, static_cast<double>(std::exp(-dt * dt)),
                            static_cast<double>(m_world.soilMoisture[ti]),
                            static_cast<double>(nutrient),
                            m_world.soilN[ti], m_world.soilP[ti], m_world.soilK[ti],
                            static_cast<double>(m_world.rainfall[ti]));
            }
            std::printf("    t%-6llu %-9s (%6.2f,%6.2f) v(%+.3f,%+.3f) hd%+.2f E%6.1f H%6.1f "
                        "hun%.2f eat%+.2f acc%+.2f | bio%7.1f wd%5.2f T%5.1f\n",
                        static_cast<unsigned long long>(m_tick),
                        actionName(static_cast<Action>(m_agents.m_action[slot])),
                        static_cast<double>(m_agents.m_x[slot]),
                        static_cast<double>(m_agents.m_y[slot]),
                        static_cast<double>(m_agents.m_vx[slot]),
                        static_cast<double>(m_agents.m_vy[slot]),
                        static_cast<double>(m_agents.m_heading[slot]),
                        static_cast<double>(m_agents.m_energy[slot]),
                        static_cast<double>(m_agents.m_hydration[slot]),
                        static_cast<double>(m_agents.m_drives[slot][Drive::Hunger]),
                        static_cast<double>(m_agents.m_outputs[slot * kBrainOutputCount + Out_Eat]),
                        static_cast<double>(m_agents.m_outputs[slot * kBrainOutputCount + Out_Accelerate]),
                        static_cast<double>(bio), static_cast<double>(wd), static_cast<double>(temp));
            (void)ph;
            std::fflush(stdout);
        }
        if (verbose && reportEvery && m_tick % reportEvery == 0) {
            const double el = secondsSince(tSim);
            const double simYears = static_cast<double>(m_tick) / static_cast<double>(kHoursPerYear);
            m_agents.recomputeStats(m_tick);
            const PopulationStats& ps = m_agents.stats();
            std::printf("  yr %6.0f  %7.0f t/s  T %5.1f  pop %5u  births %6u  deaths %6u  "
                        "age %5.1f  neurons %5.1f  syn %5.1f  F %.3f  orn %.2f\n",
                        simYears, static_cast<double>(m_tick) / (el > 0.0 ? el : 1.0),
                        m_world.stats().meanTemperature,
                        ps.population, ps.births, ps.deaths, ps.meanAgeYears,
                        ps.meanBrainNodes, ps.meanBrainConns,
                        m_agents.populationGenetics().meanInbreedingF, ps.meanOrnament);
            std::fflush(stdout);
        }
    }

    m_world.recomputeStats(m_jobs);
    sampleTelemetry();

    bool ok = true;
    if (!snapshotPath.empty()) {
        ok = saveSnapshotInternal(snapshotPath) && ok;
        if (verbose) std::printf("Snapshot -> %s\n", snapshotPath.c_str());
    }
    if (!csvPath.empty()) {
        ok = exportTelemetryCsv(csvPath) && ok;
        if (verbose) std::printf("Telemetry -> %s\n", csvPath.c_str());
    }
    if (verbose) {
        const PopulationStats& ps = m_agents.stats();
        std::printf("\nFinal population %u   (%u born, %u died)\n",
                    ps.population, ps.births, ps.deaths);
        std::printf("Deaths by cause:");
        bool any = false;
        for (int i = 1; i < static_cast<int>(DeathCause::Count); ++i) {
            if (ps.deathsByCause[i] == 0) continue;
            std::printf("  %s=%u", deathCauseName(static_cast<DeathCause>(i)), ps.deathsByCause[i]);
            any = true;
        }
        std::printf("%s\n", any ? "" : "  (none)");
        std::printf("Stage counts:");
        for (int i = 0; i < static_cast<int>(LifeStage::Count) - 1; ++i)
            std::printf("  %s=%u", lifeStageName(static_cast<LifeStage>(i)), ps.byStage[i]);
        std::printf("\nMean lifespan at death %.2f yr   pair bonds %u   pregnancies %u\n",
                    ps.meanLifespanAtDeath, ps.pairBonds, ps.pregnancies);
        // Where a population fails to replace itself, this says at which step:
        // never meeting, refusing each other, being the same gamete type, or
        // failing to conceive.
        std::printf("Courtship funnel: %u attempted -> %u mutual -> %u same-gamete -> "
                    "%u conceptions\n",
                    ps.courtshipsAttempted, ps.courtshipsMutual, ps.matingsSameType,
                    ps.conceptions);

        // Heritability, for the traits where it means something. Reported because
        // it is the number that says whether selection can move a trait at all:
        // a trait with high variance but near-zero additive variance looks
        // variable and will not budge.
        {
            const Genetics::PopulationGenetics& pg = m_agents.populationGenetics();
            if (pg.sampleSize >= 2) {
                std::vector<int> order;
                for (int t = 0; t < kTraitCount; ++t)
                    if (pg.heritability[t].codingLoci >= 2) order.push_back(t);
                std::sort(order.begin(), order.end(), [&](int a, int b) {
                    return pg.heritability[a].narrowH2 > pg.heritability[b].narrowH2;
                });
                std::printf("Heritability of the polygenic traits (h2 narrow / H2 broad, "
                            "n=%u):\n", pg.sampleSize);
                for (size_t k = 0; k < order.size() && k < 8; ++k) {
                    const int t = order[k];
                    std::printf("  %-22s h2 %.3f   H2 %.3f   Va %.4g  Vg %.4g  Vp %.4g%s\n",
                                traitSpec(static_cast<Trait>(t)).name,
                                pg.heritability[t].narrowH2, pg.heritability[t].broadH2,
                                pg.heritability[t].additiveVariance,
                                pg.heritability[t].genotypicVariance,
                                pg.heritability[t].phenotypicVariance,
                                pg.heritability[t].rangeLimited
                                    ? "   [clamped against its range]" : "");
                }
            }
        }

        // Speciation. Reported before culture because it is the slower clock:
        // a lineage split is measured in centuries where a discovery is measured
        // in years.
        const Speciation& sp = m_agents.speciation();
        std::printf("Species: %u extant of %zu ever named", sp.extantCount(),
                    sp.species().size());
        if (ps.hybridConceptions > 0)
            std::printf("  |  %u crosses paid a hybrid penalty, %u were prevented by it",
                        ps.hybridConceptions, ps.hybridBlocked);
        std::printf("\n");
        for (const SpeciesRecord& s : sp.species()) {
            std::printf("  %-28s %s  born yr %6.1f  pop %5u  peak %5u  drift %.3f",
                        s.name.c_str(), s.extant ? "extant " : "EXTINCT",
                        static_cast<double>(s.firstTick) / 8640.0,
                        s.population, s.peakPopulation, s.drift);
            if (s.parentId != 0) {
                const SpeciesRecord* p = sp.find(s.parentId);
                std::printf("  <- %s at d=%.3f", p ? p->name.c_str() : "?", s.splitDistance);
            }
            std::printf("\n");
        }

        // Culture, reported separately from genetics because it is transmitted
        // separately. `holders` reaching zero means the world has LOST a thing
        // it once knew and has to find it again from nothing.
        // The economy, if there is one. When there is not, this says so in one
        // line rather than printing a table of zeroes.
        if (!m_economy.active()) {
            std::printf("Economy: none. The module was never entered.\n");
        } else {
            std::printf("Economy: %llu exchanges from %llu willing encounters "
                        "(%.1f%% coincidence rate)\n",
                        static_cast<unsigned long long>(m_economy.totalExchanges()),
                        static_cast<unsigned long long>(m_economy.totalOffers()),
                        m_economy.coincidenceRate() * 100.0);
            if (m_economy.hasCurrency()) {
                const double gini = m_economy.giniCoefficient(m_agents);
                std::printf("  currency: %s", m_economy.currencyName().c_str());
                if (gini >= 0.0) std::printf("   wealth Gini %.3f", gini);
                std::printf("\n");
            }
            else
                std::printf("  no currency has emerged and none was decreed\n");
            std::vector<GoodStats> gs = m_economy.goods();
            std::sort(gs.begin(), gs.end(), [](const GoodStats& a, const GoodStats& b) {
                return a.moneyness > b.moneyness;
            });
            for (size_t i = 0; i < gs.size() && i < 8; ++i) {
                const Substance* sub = chem().substance(gs[i].substance);
                std::printf("  %-26s trades %6llu  taken %6llu  passed on %5.1f%%  "
                            "moneyness %.3f\n",
                            sub ? sub->name.c_str() : "?",
                            static_cast<unsigned long long>(gs[i].trades),
                            static_cast<unsigned long long>(gs[i].acquiredByTrade),
                            gs[i].acquiredByTrade
                                ? 100.0 * static_cast<double>(gs[i].passedOn) /
                                  static_cast<double>(gs[i].acquiredByTrade)
                                : 0.0,
                            gs[i].moneyness);
            }
        }

        const KnowledgeBase& kb = m_agents.knowledge();
        std::printf("Culture: %zu first discoveries, %u knowledge units held, "
                    "technology index %.3f\n",
                    kb.discoveries().size(), ps.knowledgeUnits, ps.technologyIndex);
        std::printf("Techniques reached:");
        bool anyTech = false;
        for (int t = 0; t < static_cast<int>(Technique::Count); ++t) {
            uint32_t n = 0;
            for (uint32_t slot : m_agents.liveSlots())
                if (kb.hasTechnique(slot, static_cast<Technique>(t))) ++n;
            if (n == 0) continue;
            std::printf("  %s=%u", techniqueName(static_cast<Technique>(t)), n);
            anyTech = true;
        }
        std::printf("%s\n", anyTech ? "" : "  (none)");
        for (const DiscoveryRecord& d : kb.discoveries()) {
            const Reaction* rx = chem().reaction(d.reactionId);
            std::printf("  tick %8llu  %-44s by %-16s holders %u%s\n",
                        static_cast<unsigned long long>(d.firstTick),
                        rx ? rx->name.c_str() : "?",
                        d.discovererName.empty() ? "(unknown)" : d.discovererName.c_str(),
                        d.holders, d.holders == 0 ? "  [LOST]" : "");
        }
    }
    if (verbose) {
        // The profiling report. Printed for every headless run, because a claim
        // about performance without a per-stage breakdown is not a measurement.
        std::printf("\nWhere the tick goes (%llu ticks profiled):\n",
                    static_cast<unsigned long long>(m_profiler.ticks()));
        const double tickMs = m_profiler.meanTickMs();
        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            const Stage st = static_cast<Stage>(i);
            if (st == Stage::Render) continue;
            const double mean = m_profiler.meanMs(st);
            if (mean <= 0.0) continue;
            std::printf("  %-16s %9.4f ms  %5.1f%%   peak %8.3f   total %7.2f s\n",
                        stageName(st), mean, tickMs > 0.0 ? 100.0 * mean / tickMs : 0.0,
                        m_profiler.peakMs(st), m_profiler.totalMs(st) / 1000.0);
        }
        std::printf("  %-16s %9.4f ms           dominant stage: %s\n", "TOTAL TICK", tickMs,
                    stageName(m_profiler.dominant()));
    }
    if (verbose) {
        const double el = secondsSince(tSim);
        std::printf("Simulated %.1f years (%llu ticks) in %.2f s = %.0f ticks/s = %.0fx\n",
                    years, static_cast<unsigned long long>(totalTicks), el,
                    static_cast<double>(totalTicks) / (el > 0.0 ? el : 1.0),
                    static_cast<double>(totalTicks) / (el > 0.0 ? el : 1.0));
        std::fflush(stdout);
    }

    m_jobs.shutdown();
    return ok;
}

}  // namespace gen
