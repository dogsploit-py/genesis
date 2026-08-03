#include "core/profiler.h"

namespace gen {

const char* stageName(Stage s) {
    switch (s) {
        case Stage::Disasters:    return "disasters";
        case Stage::Clock:        return "clock";
        case Stage::Thermal:      return "thermal";
        case Stage::Hydrology:    return "hydrology";
        case Stage::Ecology:      return "ecology";
        case Stage::Weather:      return "weather";
        case Stage::Geology:      return "geology";
        case Stage::SpatialIndex: return "spatial index";
        case Stage::Sense:        return "sense";
        case Stage::Think:        return "think";
        case Stage::Neighbours:   return "neighbour lists";
        case Stage::Act:          return "act";
        case Stage::Physics:      return "physics";
        case Stage::Metabolism:   return "metabolism";
        case Stage::Reproduction: return "reproduction";
        case Stage::Chemistry:    return "chemistry";
        case Stage::Economy:      return "economy";
        case Stage::Reap:         return "reap dead";
        case Stage::Species:      return "species";
        case Stage::Telemetry:    return "telemetry";
        case Stage::PopGenetics:  return "pop genetics";
        case Stage::Autosave:     return "autosave";
        case Stage::Render:       return "render (not a tick stage)";
        case Stage::Count:        break;
    }
    return "?";
}

const char* stageNote(Stage s) {
    switch (s) {
        case Stage::Disasters:
            return "Persistent disasters -- ice ages, volcanic winters, spreading fires. Resolved "
                   "before the environment so their offsets apply to this tick's climate.";
        case Stage::Clock:
            return "Calendar rollovers and insolation. Trivially cheap; listed so the tick order "
                   "in the profiler matches the tick order in the architecture document.";
        case Stage::Thermal:
            return "Temperature relaxation and lateral diffusion, every 6 ticks. Periodic because "
                   "that is the timescale the process actually has, not to save time.";
        case Stage::Hydrology:
            return "Rainfall, infiltration, runoff routed down the D8 drainage, evaporation. "
                   "Daily.";
        case Stage::Ecology:
            return "Plant growth against water, temperature and soil nutrients, with uptake that "
                   "conserves mass. Daily.";
        case Stage::Weather:
            return "Pressure systems and the moisture field. Monthly.";
        case Stage::Geology:
            return "Uplift, subsidence and slow erosion. Yearly.";
        case Stage::SpatialIndex:
            return "Counting-sort rebuild of the uniform grid. O(n) and allocation-free after the "
                   "first build; everything that asks about neighbours depends on it.";
        case Stage::Sense:
            return "Builds all 48 brain inputs for every agent: vision over six sectors at three "
                   "ranges, interoception, social and spatial memory. Parallel.";
        case Stage::Think:
            return "Evaluates every brain, every tick, at every speed. No level of detail and no "
                   "cohorts. Parallel.";
        case Stage::Neighbours:
            return "Close-range neighbour lists in CSR form, built in parallel so the serial act "
                   "stage does not have to run three spatial queries per agent over the same "
                   "radius. Valid for the whole stage because positions are not integrated until "
                   "physics, which runs after it.";
        case Stage::Act:
            return "Applies motor outputs. SERIAL by necessity: eating removes biomass from the "
                   "world, so ordering has to be fixed for reproducibility.";
        case Stage::Physics:
            return "Movement integration with wall sliding. Parallel.";
        case Stage::Metabolism:
            return "Energy, hydration, thermoregulation, damage, aging, drives and the reward "
                   "signal. Parallel.";
        case Stage::Reproduction:
            return "Courtship resolution, gestation and birth. SERIAL: a mating mutates both "
                   "parties and can create a third agent.";
        case Stage::Chemistry:
            return "Gathering, experiments and teaching. SERIAL: substances leave the ground and "
                   "knowledge crosses between individuals.";
        case Stage::Economy:
            return "Barter and price formation. Costs literally nothing until a currency exists, "
                   "because the stage is not entered at all.";
        case Stage::Reap:
            return "Corpse nutrient return and slot recycling.";
        case Stage::Species:
            return "Clusters the population by neutral genetic distance and detects splits. "
                   "Periodic, and a reporting cost rather than a simulation one.";
        case Stage::Telemetry:
            return "World and population statistics plus the time series sample. Periodic.";
        case Stage::PopGenetics:
            return "Heterozygosity, Tajima's D, Ne, Fst. Sampled above a threshold -- a reporting "
                   "shortcut, never a simulation one.";
        case Stage::Autosave:
            return "Snapshot write on the autosave interval.";
        case Stage::Render:
            return "CPU rasterisation of the viewport. Not part of the tick; shown so you can see "
                   "what hiding the render buys you.";
        case Stage::Count: break;
    }
    return "";
}

bool stageIsPeriodic(Stage s) {
    switch (s) {
        case Stage::Thermal:
        case Stage::Hydrology:
        case Stage::Ecology:
        case Stage::Weather:
        case Stage::Geology:
        case Stage::Species:
        case Stage::Telemetry:
        case Stage::PopGenetics:
        case Stage::Autosave:
            return true;
        default:
            return false;
    }
}

}  // namespace gen
