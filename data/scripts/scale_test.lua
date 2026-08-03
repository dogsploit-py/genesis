-- data/scripts/scale_test.lua
--
-- The M9 scaling test: 10,000 agents, all simulated in full.
--
-- No level of detail, no cohorts, no "distant agents resolved statistically".
-- Every one of these individuals gets all 48 sensory inputs built, its whole
-- brain evaluated, its metabolism integrated and its drives updated, on every
-- tick. That is the constraint the architecture document commits to, and the
-- point of the test is to find out what it costs.
--
-- Death is switched off so the population stays at the target for the whole
-- measurement rather than decaying out from under it.

config.set("rules.disable_death", true)
config.set("species.period_ticks", 0)                  -- measure the tick, not the reporting
config.set("stats.period_ticks", 0)
config.set("knowledge.technique_discovery_rate", 0.0)

local TARGET = 10000

local w, h = world.size()
local placed, tries = 0, 0
-- A coprime stride over the map so the founders spread out instead of piling
-- into one corner: density drives the spatial-hash cost, and a test that put
-- everyone in one place would measure the wrong thing.
while placed < TARGET and tries < TARGET * 40 do
    tries = tries + 1
    local x = (tries * 97) % w
    local y = (tries * 61) % h
    local t = world.tile(x, y)
    if t.elevation > 0 then
        local uid = agent.spawn(x, y, (placed % 2) == 0)
        if uid and uid > 0 then placed = placed + 1 end
    end
end

print(string.format("placed %d of %d agents over %d attempts", placed, TARGET, tries))
