-- data/scripts/speciation_test.lua
--
-- Verifies that speciation is DETECTED, that it is attached to the right branch
-- of the phylogeny, and that it has reproductive consequences.
--
-- A split that arises purely by drift needs two subpopulations, no gene flow,
-- and centuries of simulated time -- the neutral clock is slow on purpose. So
-- this script does the divine thing instead and shifts one group's neutral loci
-- directly, which is exactly the kind of intervention god mode is for. What it
-- does NOT do is tell the detector anything: nothing here touches the species
-- machinery. The detector is handed a population and has to work out for itself
-- that there are now two lineages in it, how far apart they are, and which came
-- from which.
--
-- Everything downstream of that is genuine: the clustering, the parent
-- attachment, the naming, the phylogeny events, and the hybrid fertility
-- penalty all run on measured genomes.

config.set("rules.disable_death", true)
config.set("knowledge.technique_discovery_rate", 0.0)   -- keep the output about species
config.set("species.period_ticks", 720)
config.set("species.min_founders", 10)

local w, h = world.size()

-- Spirals outward from the requested centre until it finds enough habitable
-- land. A fixed box does not work: whether there is any land at a given map
-- coordinate depends entirely on the seed.
local function placeCluster(cx, cy, want)
    local uids = {}
    local radius = 6
    while #uids < want and radius < math.min(w, h) / 2 do
        for tries = 1, 3000 do
            if #uids >= want then break end
            local x = (cx + (tries * 13) % (radius * 2 + 1) - radius) % w
            local y = (cy + (tries * 29) % (radius * 2 + 1) - radius) % h
            local t = world.tile(x, y)
            if t.elevation > 0 and t.biomass > 10 then
                local uid = agent.spawn(x, y, (#uids % 2) == 0)
                if uid and uid > 0 then uids[#uids + 1] = uid end
            end
        end
        radius = radius * 2
    end
    return uids
end

local west = placeCluster(math.floor(w * 0.20), math.floor(h * 0.28), 70)
local east = placeCluster(math.floor(w * 0.80), math.floor(h * 0.72), 70)
print(string.format("west %d, east %d", #west, #east))

-- Shift the eastern group's NEUTRAL loci by a fixed offset. Junk and MHC only:
-- touching coding loci would change what these individuals are like, and the
-- point is that they are physically identical and merely genetically distant.
local SHIFT = 3.0
local shifted = 0
for _, uid in ipairs(east) do
    local n = agent.geneCount(uid)
    for i = 1, n do
        local g = agent.gene(uid, i)
        if g.typeName == "Junk" or g.typeName == "MHC" then
            agent.setGene(uid, i, "alleleA", g.alleleA + SHIFT)
            agent.setGene(uid, i, "alleleB", g.alleleB + SHIFT)
            shifted = shifted + 1
        end
    end
end
print(string.format("shifted %d neutral alleles in the eastern group by %+.1f", shifted, SHIFT))
