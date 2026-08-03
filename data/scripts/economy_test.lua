-- data/scripts/economy_test.lua
--
-- Brings an economy into existence and lets it run, to check that barter works
-- and that the money detector has something real to measure.
--
-- Note what this does NOT do: it does not give anyone anything, it does not set
-- a price, and it does not tell any agent to trade. It enables exchange and then
-- gets out of the way. Every trade in the resulting run happened because two
-- individuals each valued what the other was carrying more than what they were
-- carrying themselves.

config.set("rules.disable_death", true)
config.set("species.period_ticks", 0)          -- keep the output about the economy
config.set("econ.detect_period_ticks", 720)    -- look for money often enough to see it

-- Discovery drives valuation: a substance is worth something to you largely
-- because you know a reaction that consumes it. So the technique ladder needs to
-- be moving for there to be gains from trade at all.
config.set("knowledge.technique_discovery_rate", 0.002)

local w, h = world.size()
local placed, tries = 0, 0
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
print(string.format("placed %d founders", placed))

-- Exchange becomes possible here, and not before.
god.act{kind = "Enable barter"}
print("barter enabled -- nobody was given anything")
