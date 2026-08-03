-- data/scripts/chemistry_test.lua
--
-- Verification harness for M6. The point is to check that DISCOVERY WORKS, not
-- to check demography, so this deliberately removes the demographic failure
-- from the picture: death is switched off and a large founding population is
-- placed, leaving a stable set of experimenters.
--
-- Nothing here tells an agent what to make. Every discovery in the resulting
-- run is found by an agent combining what it picked up off the ground and
-- seeing what the reaction engine does with it.

config.set("rules.disable_death", true)

-- A generous discovery rate, so the technique ladder is visible over a run of
-- decades rather than of millennia. The default is far lower on purpose.
config.set("knowledge.technique_discovery_rate", 0.002)

local w, h = world.size()
local placed = 0
local tries = 0
while placed < 240 and tries < 20000 do
    tries = tries + 1
    local x = (tries * 37) % w
    local y = (tries * 61) % h
    local t = world.tile(x, y)
    if t.elevation > 0 and t.biomass > 20 then
        local uid = agent.spawn(x, y, (placed % 2) == 0)
        if uid and uid > 0 then placed = placed + 1 end
    end
end

print(string.format("placed %d founders over %d attempts", placed, tries))
