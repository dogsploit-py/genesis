#include "core/config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gen {

Config& Config::instance() {
    static Config c;
    return c;
}

void Config::splitKey(const std::string& key, std::string& section, std::string& name) {
    const size_t dot = key.find('.');
    if (dot == std::string::npos) {
        section = "general";
        name = key;
    } else {
        section = key.substr(0, dot);
        name = key.substr(dot + 1);
    }
}

int Config::indexOf(const char* key) const {
    auto it = m_index.find(key);
    return (it == m_index.end()) ? -1 : it->second;
}

CfgEntry& Config::add(const char* key, CfgType type, const char* desc) {
    const int existing = indexOf(key);
    if (existing >= 0) return m_entries[static_cast<size_t>(existing)];

    CfgEntry e;
    e.key = key;
    splitKey(e.key, e.section, e.name);
    e.description = desc ? desc : "";
    e.type = type;
    m_entries.push_back(e);
    const int idx = static_cast<int>(m_entries.size()) - 1;
    m_index[e.key] = idx;
    return m_entries[static_cast<size_t>(idx)];
}

void Config::registerBool(const char* key, bool def, const char* desc) {
    CfgEntry& e = add(key, CfgType::Bool, desc);
    e.value = e.defaultValue = def ? 1.0 : 0.0;
    e.minValue = 0.0;
    e.maxValue = 1.0;
}

void Config::registerInt(const char* key, int64_t def, int64_t lo, int64_t hi,
                         const char* desc, bool logarithmic) {
    CfgEntry& e = add(key, CfgType::Int, desc);
    e.value = e.defaultValue = static_cast<double>(def);
    e.minValue = static_cast<double>(lo);
    e.maxValue = static_cast<double>(hi);
    e.logarithmic = logarithmic;
}

void Config::registerFloat(const char* key, double def, double lo, double hi,
                           const char* desc, bool logarithmic) {
    CfgEntry& e = add(key, CfgType::Float, desc);
    e.value = e.defaultValue = def;
    e.minValue = lo;
    e.maxValue = hi;
    e.logarithmic = logarithmic;
}

void Config::registerString(const char* key, const char* def, const char* desc) {
    CfgEntry& e = add(key, CfgType::String, desc);
    e.strValue = e.strDefault = def ? def : "";
}

bool Config::getBool(const char* key, bool fallback) const {
    const int i = indexOf(key);
    return (i < 0) ? fallback : (m_entries[static_cast<size_t>(i)].value != 0.0);
}

int64_t Config::getInt(const char* key, int64_t fallback) const {
    const int i = indexOf(key);
    return (i < 0) ? fallback : static_cast<int64_t>(m_entries[static_cast<size_t>(i)].value);
}

double Config::getFloat(const char* key, double fallback) const {
    const int i = indexOf(key);
    return (i < 0) ? fallback : m_entries[static_cast<size_t>(i)].value;
}

const std::string& Config::getString(const char* key) const {
    static const std::string kEmpty;
    const int i = indexOf(key);
    return (i < 0) ? kEmpty : m_entries[static_cast<size_t>(i)].strValue;
}

void Config::setBool(const char* key, bool v) {
    const int i = indexOf(key);
    if (i >= 0) m_entries[static_cast<size_t>(i)].value = v ? 1.0 : 0.0;
}

void Config::setInt(const char* key, int64_t v) {
    const int i = indexOf(key);
    if (i >= 0) m_entries[static_cast<size_t>(i)].value = static_cast<double>(v);
}

void Config::setFloat(const char* key, double v) {
    const int i = indexOf(key);
    if (i >= 0) m_entries[static_cast<size_t>(i)].value = v;
}

void Config::setString(const char* key, const std::string& v) {
    const int i = indexOf(key);
    if (i >= 0) m_entries[static_cast<size_t>(i)].strValue = v;
}

void Config::restoreDefaults() {
    for (CfgEntry& e : m_entries) {
        e.value = e.defaultValue;
        e.strValue = e.strDefault;
    }
}

void Config::restoreDefaults(const std::string& section) {
    for (CfgEntry& e : m_entries) {
        if (e.section != section) continue;
        e.value = e.defaultValue;
        e.strValue = e.strDefault;
    }
}

std::vector<std::string> Config::sections() const {
    std::vector<std::string> out;
    for (const CfgEntry& e : m_entries)
        if (std::find(out.begin(), out.end(), e.section) == out.end())
            out.push_back(e.section);
    return out;
}

namespace {
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}
}  // namespace

bool Config::load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    std::string section = "general";
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = trim(s.substr(0, eq));
        const std::string val  = trim(s.substr(eq + 1));
        const std::string key  = section + "." + name;

        const int i = indexOf(key.c_str());
        // Silently ignore unknown keys: a config written by a newer build, or
        // hand-edited with a typo, must not prevent the program from starting.
        if (i < 0) continue;
        CfgEntry& e = m_entries[static_cast<size_t>(i)];
        switch (e.type) {
            case CfgType::Bool:
                e.value = (val == "1" || val == "true" || val == "yes" || val == "on") ? 1.0 : 0.0;
                break;
            case CfgType::Int:
                e.value = static_cast<double>(std::strtoll(val.c_str(), nullptr, 10));
                break;
            case CfgType::Float:
                e.value = std::strtod(val.c_str(), nullptr);
                break;
            case CfgType::String:
                e.strValue = val;
                break;
        }
        // Clamp on load so a bad file cannot put the sim into an invalid state.
        if (e.isNumeric() && e.maxValue > e.minValue) {
            if (e.value < e.minValue) e.value = e.minValue;
            if (e.value > e.maxValue) e.value = e.maxValue;
        }
    }
    std::fclose(f);
    m_lastLoadPath = path;
    m_loadedFromFile = true;
    return true;
}

bool Config::save(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    std::fprintf(f, "# GENESIS configuration\n");
    std::fprintf(f, "# Auto-generated from the constant registry (core/config.cpp).\n");
    std::fprintf(f, "# Every value the simulation uses is here; nothing is hardcoded in logic.\n\n");

    for (const std::string& sec : sections()) {
        std::fprintf(f, "[%s]\n", sec.c_str());
        for (const CfgEntry& e : m_entries) {
            if (e.section != sec) continue;
            if (!e.description.empty()) std::fprintf(f, "# %s\n", e.description.c_str());
            switch (e.type) {
                case CfgType::Bool:
                    std::fprintf(f, "%s = %s\n", e.name.c_str(), e.value != 0.0 ? "true" : "false");
                    break;
                case CfgType::Int:
                    std::fprintf(f, "%s = %lld\n", e.name.c_str(),
                                 static_cast<long long>(e.value));
                    break;
                case CfgType::Float:
                    std::fprintf(f, "%s = %.10g\n", e.name.c_str(), e.value);
                    break;
                case CfgType::String:
                    std::fprintf(f, "%s = %s\n", e.name.c_str(), e.strValue.c_str());
                    break;
            }
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// The registry itself. Every constant GENESIS uses is declared here exactly
// once. Milestones append to this function; the Settings UI updates itself.
// ---------------------------------------------------------------------------
void registerAllSettings() {
    Config& c = Config::instance();

    // -- world ---------------------------------------------------------------
    c.registerInt("world.width", 1024, 128, 4096,
                  "World width in tiles. Memory is ~64 bytes/tile: 1024 = 67 MB, 4096 = 1.07 GB.");
    c.registerInt("world.height", 1024, 128, 4096,
                  "World height in tiles. Same memory note as world.width.");
    c.registerFloat("world.tile_metres", 50.0, 1.0, 5000.0,
                    "Edge length of one tile in metres. Sets the physical scale of everything.");
    c.registerFloat("world.sea_level", 0.0, -2000.0, 2000.0,
                    "Elevation in metres that counts as sea level.");
    c.registerFloat("world.latitude_span", 120.0, 10.0, 180.0,
                    "Degrees of latitude spanned top to bottom. Drives insolation and seasons.");

    // -- worldgen ------------------------------------------------------------
    c.registerInt("worldgen.plates", 12, 2, 64,
                  "Number of tectonic plates seeded. Boundaries become mountain belts and faults.");
    c.registerFloat("worldgen.continent_fraction", 0.34, 0.05, 0.95,
                    "Target fraction of the map above sea level, matched by quantile.");
    c.registerInt("worldgen.noise_octaves", 8, 1, 12,
                  "fBm octaves for base terrain. More octaves = finer detail, slower generation.");
    c.registerFloat("worldgen.noise_lacunarity", 2.0, 1.2, 4.0,
                    "Frequency multiplier between successive fBm octaves.");
    c.registerFloat("worldgen.noise_gain", 0.5, 0.1, 0.95,
                    "Amplitude multiplier between successive fBm octaves (persistence).");
    c.registerFloat("worldgen.noise_scale", 0.0025, 0.0002, 0.05,
                    "Base spatial frequency of terrain noise, in cycles per tile.", true);
    c.registerFloat("worldgen.oceanic_baseline", -3800.0, -8000.0, 0.0,
                    "Baseline elevation of oceanic crust in metres. Dense basaltic crust floats "
                    "low on the mantle, which is why ocean basins are deep.");
    c.registerFloat("worldgen.continental_baseline", 700.0, -1000.0, 4000.0,
                    "Baseline elevation of continental crust in metres. Less dense than oceanic "
                    "crust, so it floats higher.");
    c.registerFloat("worldgen.oceanic_plate_fraction", 0.55, 0.0, 1.0,
                    "Probability that a given tectonic plate is oceanic rather than continental.");
    c.registerFloat("worldgen.uplift_strength", 1800.0, 0.0, 6000.0,
                    "Peak orogenic uplift in metres at a convergent plate boundary.");
    c.registerFloat("worldgen.mountain_falloff", 28.0, 2.0, 200.0,
                    "Distance in tiles over which plate-boundary uplift decays.");
    c.registerInt("worldgen.erosion_passes", 60, 0, 600,
                  "Hydraulic + thermal erosion iterations during generation. Carves valleys.");
    c.registerFloat("worldgen.erosion_rate", 0.22, 0.0, 1.0,
                    "Fraction of the computed slope-driven material moved per erosion pass.");
    c.registerFloat("worldgen.river_threshold", 220.0, 10.0, 100000.0,
                    "Upstream flow accumulation (in tiles) above which a channel counts as a river.", true);
    c.registerFloat("worldgen.ore_richness", 1.0, 0.0, 8.0,
                    "Global multiplier on ore deposit abundance from all emplacement processes.");
    c.registerInt("worldgen.strata_layers", 6, 1, 6,
                  "Number of stacked rock strata stored per tile, surface-first.");

    // -- climate -------------------------------------------------------------
    c.registerFloat("climate.base_temperature", 14.0, -40.0, 60.0,
                    "Global AREA-MEAN surface temperature at sea level, degrees C. The latitude "
                    "profile is constructed so its area average is exactly this value.");
    c.registerFloat("climate.pole_gradient", 40.0, 0.0, 90.0,
                    "Equator-to-pole temperature contrast in degrees C. Earth's is about 40: "
                    "roughly +27 C at the equator against -13 C at the pole.");
    c.registerFloat("climate.lapse_rate", 6.5, 0.0, 12.0,
                    "Atmospheric temperature drop in degrees C per 1000 m of elevation.");
    c.registerFloat("climate.seasonal_amplitude", 12.0, 0.0, 45.0,
                    "Peak summer-to-winter temperature swing at high latitude, degrees C.");
    c.registerFloat("climate.diurnal_amplitude", 6.0, 0.0, 30.0,
                    "Peak day-to-night temperature swing, degrees C.");
    c.registerFloat("climate.axial_tilt", 23.44, 0.0, 60.0,
                    "Planetary axial tilt in degrees. Zero removes seasons entirely.");
    c.registerFloat("climate.base_rainfall", 900.0, 0.0, 6000.0,
                    "Baseline annual precipitation in mm before orographic and latitude effects.");
    c.registerFloat("climate.orographic_strength", 1.4, 0.0, 5.0,
                    "How strongly windward slopes gain rain and lee slopes lose it (rain shadow).");
    c.registerFloat("climate.prevailing_wind_deg", 270.0, 0.0, 360.0,
                    "Direction the prevailing wind blows FROM, degrees clockwise from north.");
    c.registerFloat("climate.ocean_moderation", 0.55, 0.0, 1.0,
                    "How much large water bodies damp the local temperature range.");

    // -- environment update schedule ----------------------------------------
    c.registerInt("env.thermal_period_ticks", 6, 1, 8640,
                  "Ticks between tile thermal relaxation steps. Matches the soil/rock time constant.");
    c.registerInt("env.hydrology_period_ticks", 24, 1, 8640,
                  "Ticks between evaporation / soil moisture / water table updates (default: daily).");
    c.registerInt("env.ecology_period_ticks", 24, 1, 8640,
                  "Ticks between plant growth and seed dispersal updates (default: daily).");
    c.registerInt("env.weather_period_ticks", 720, 1, 86400,
                  "Ticks between rainfall pattern and flow accumulation refresh (default: monthly).");
    c.registerInt("env.geology_period_ticks", 8640, 1, 864000,
                  "Ticks between erosion, sedimentation and glaciation steps (default: yearly).");
    c.registerBool("env.enable_erosion", true,
                   "Run long-timescale erosion and sediment transport during the simulation.");
    c.registerBool("env.enable_glaciation", true,
                   "Allow ice sheets to form and retreat at high latitude and elevation.");

    // -- ecology -------------------------------------------------------------
    c.registerFloat("ecology.plant_growth_rate", 0.25, 0.0, 1.0,
                    "Logistic growth rate of plant biomass per daily step, at optimal conditions.");
    c.registerFloat("ecology.plant_max_biomass", 800.0, 1.0, 20000.0,
                    "Carrying capacity of plant biomass per tile, in kg.");
    c.registerFloat("ecology.optimal_temperature", 22.0, -20.0, 50.0,
                    "Temperature in degrees C at which photosynthesis peaks.");
    c.registerFloat("ecology.temperature_tolerance", 16.0, 1.0, 60.0,
                    "Width of the photosynthesis temperature response curve, degrees C.");
    c.registerFloat("ecology.nutrient_uptake", 0.30, 0.0, 4.0,
                    "Soil N/P/K drawn down per unit of biomass grown.");
    c.registerFloat("ecology.decay_return_rate", 0.003, 0.0, 1.0,
                    "Fraction of dead biomass returned to soil nutrients per daily step.");

    // -- simulation loop -----------------------------------------------------
    c.registerFloat("sim.batch_target_ms", 2.0, 0.1, 50.0,
                    "Wall-clock budget per sim tick batch. Larger = less locking, coarser UI latency.");
    c.registerInt("sim.worker_threads", 0, 0, 127,
                  "Background worker threads. 0 = auto (hardware_concurrency - 1). Baked into saves.");
    c.registerInt("sim.autosave_years", 25, 0, 10000,
                  "Autosave every N simulated years. 0 disables autosave.");
    c.registerInt("sim.event_log_capacity", 20000, 100, 1000000,
                  "Maximum world events retained in the ring buffer shown by the Event Feed.");
    c.registerInt("sim.telemetry_period_ticks", 720, 1, 86400,
                  "Ticks between time-series chart samples (default: monthly).");
    c.registerInt("sim.telemetry_capacity", 60000, 100, 2000000,
                  "Maximum samples retained per time series before the oldest are dropped.");

    // -- agents (M2) ---------------------------------------------------------
    c.registerInt("sim.max_agents", 12000, 64, 200000,
                  "Maximum simultaneous agents. Slots are allocated up front and never grow, so "
                  "this sets the memory ceiling: roughly 22 KB per slot for genome, brain and body.",
                  true);
    c.registerInt("sim.founder_count", 20, 2, 5000,
                  "Founding population placed at world creation.");
    c.registerFloat("sim.founder_age_years", 14.0, 0.0, 200.0,
                    "Age the founders are created at. They arrive as established adults so a run "
                    "does not open with a decade of waiting before anyone can breed.");
    c.registerFloat("sim.founder_spread", 10.0, 1.0, 2000.0,
                    "Radius in tiles the founders are scattered over. A tight cluster means a "
                    "severe founder effect and a lot of early inbreeding.");
    c.registerInt("sim.pedigree_capacity", 400000, 1000, 20000000,
                  "Ancestry records retained. The oldest are dropped past this, which only "
                  "degrades relatedness estimates for very deep lineages.", true);
    c.registerFloat("agents.spatial_cell_tiles", 8.0, 1.0, 64.0,
                    "Spatial hash cell size in tiles. Should be about the largest sensory range.");
    c.registerInt("agents.relationship_capacity", 24, 2, 256,
                  "How many other individuals an agent can remember. Finite social memory is a "
                  "real constraint on group size.");
    c.registerInt("agents.kin_depth", 5, 1, 12,
                  "Pedigree recursion depth for relatedness. Deeper is more accurate and costs "
                  "exponentially more.");
    c.registerFloat("agents.base_metabolic_burn", 0.25, 0.0, 10.0,
                    "Baseline energy burned per hour before activity, at unit size. Scaled by "
                    "mass^0.75, following Kleiber's law.");
    c.registerFloat("agents.move_energy_cost", 0.5, 0.0, 20.0,
                    "Energy cost per tile moved, per unit size.");
    c.registerFloat("agents.thermoregulation_cost", 0.20, 0.0, 5.0,
                    "Energy per degree C per hour spent outside the comfortable thermal band.");
    c.registerFloat("agents.hydration_burn", 0.18, 0.0, 10.0,
                    "Water lost per hour at unit size. Roughly 5% of body water per day, which "
                    "is the mammalian figure; much above that and agents cannot travel far "
                    "enough between water sources to forage at all.");
    c.registerFloat("agents.eat_rate_kg", 1.2, 0.1, 50.0,
                    "Maximum plant biomass consumed per hour. Biomass genuinely leaves the tile.");
    c.registerFloat("agents.energy_per_kg", 9.0, 0.1, 100.0,
                    "Energy yield per kg of biomass, before digestive efficiency.");
    c.registerFloat("agents.drink_rate", 12.0, 0.1, 100.0,
                    "Hydration restored per hour while drinking.");
    c.registerFloat("agents.turn_rate", 0.9, 0.0, 3.2,
                    "Maximum heading change in radians per hour at full turn output.");
    c.registerFloat("agents.min_vigour", 0.30, 0.0, 1.0,
                    "Floor on how weak starvation and injury can make an agent. Zero reproduces "
                    "the death spiral this floor exists to prevent: with no energy an agent "
                    "cannot move, so it can never reach the food that would save it.");
    c.registerFloat("agents.drag", 0.35, 0.0, 1.0,
                    "Velocity damping per hour. High drag means movement stops as soon as thrust does.");
    c.registerFloat("agents.base_mortality", 4e-7, 0.0, 1e-3,
                    "Gompertz hazard scale: baseline probability of death per hour at age zero.", true);
    c.registerFloat("agents.gompertz_exponent", 5.5, 0.0, 20.0,
                    "How steeply the mortality hazard rises with age. The hazard multiplies by "
                    "exp(this) over a full lifespan, so lifespan is a distribution, not a cap.");
    c.registerFloat("ecology.corpse_nutrient_return", 0.6, 0.0, 2.0,
                    "Fraction of a corpse's mass returned to soil nutrients, closing the cycle.");

    // -- reproduction --------------------------------------------------------
    c.registerFloat("agents.repro_drive_rate", 0.004, 0.0, 0.5,
                    "Reproductive drive accumulated per hour in a healthy, well-fed adult. "
                    "Discharging it on a successful mating emits the large reward pulse.");
    c.registerFloat("agents.courtship_range", 6.0, 0.5, 40.0,
                    "How far apart two agents can be and still court.");
    c.registerFloat("agents.courtship_drive_threshold", 0.25, 0.0, 1.0,
                    "Reproductive drive needed before an agent will court at all.");
    c.registerFloat("agents.courtship_cooldown", 72.0, 0.0, 2000.0,
                    "Hours before an agent will attempt courtship again, whatever the outcome. "
                    "Without a refractory period a pair re-evaluates every tick, is refused "
                    "within hours, and the rejection memory then locks them out for good.");
    c.registerFloat("agents.mating_energy_cost", 12.0, 0.0, 200.0,
                    "Energy each partner spends on a mating.");
    c.registerFloat("agents.mating_reward_pulse", 3.0, 0.0, 30.0,
                    "Size of the reward pulse emitted on a successful mating. Because eligibility "
                    "traces are still warm, this reinforces the whole preceding behaviour chain.");
    c.registerFloat("agents.bond_proximity_reward", 0.02, 0.0, 1.0,
                    "Sustained reward per hour for being near a bonded partner. The oxytocin "
                    "analogue: this is how monogamy and mate-guarding can evolve.");
    c.registerFloat("agents.bond_formation_chance", 0.25, 0.0, 1.0,
                    "Base probability a mating forms a lasting pair bond, scaled by how much both "
                    "partners value affiliation.");
    c.registerFloat("agents.base_conception_chance", 0.60, 0.0, 1.0,
                    "Probability a mating conceives, before fertility traits are applied.");
    c.registerFloat("agents.gestation_energy_cost", 0.5, 0.0, 20.0,
                    "Extra energy the gestating parent burns per hour.");
    c.registerFloat("agents.childbirth_mortality", 0.012, 0.0, 1.0,
                    "Base probability the gestating parent dies giving birth, modified by "
                    "hardiness and condition.");
    c.registerInt("agents.max_perceived_neighbours", 24, 1, 4096,
                  "How many other individuals one agent can take in at once. Bounded attention, "
                  "which is the realistic case: an animal in a herd of five hundred does not "
                  "individually track five hundred others, and this model already says social "
                  "MEMORY is finite. Which neighbours are perceived follows the spatial index's "
                  "traversal order, so it is fixed and reproducible but not sorted by distance.");
    c.registerInt("agents.max_interactions_per_tick", 8, 1, 4096,
                  "How many neighbours one individual can groom or share with in a single "
                  "simulated hour. A bound is the realistic case, not the unbounded one: "
                  "agents converge on water and forage, so a tile can hold dozens of them, and "
                  "without this an individual groomed every neighbour within reach every hour. "
                  "It also decides how the serial interaction stage scales -- unbounded, its "
                  "cost followed local crowding rather than population size.");
    c.registerFloat("agents.share_fraction", 0.02, 0.0, 0.5,
                    "Fraction of its own energy an agent hands to each neighbour when the share "
                    "output fires. Giving stops once the giver drops below half reserves, so "
                    "generosity is costly without being suicidal.");
    c.registerFloat("agents.attack_damage", 0.02, 0.0, 1.0,
                    "Health removed per hour of sustained attack at unit strength, before the "
                    "target's hardiness resists it.");
    c.registerFloat("agents.attack_energy_cost", 2.0, 0.0, 50.0,
                    "Energy the attacker spends per hour of fighting. Aggression has to cost "
                    "something or it is never worth not doing.");
    c.registerFloat("agents.parental_care_range", 4.0, 0.0, 40.0,
                    "How far a parent will provision a juvenile.");

    // -- genetics ------------------------------------------------------------
    c.registerInt("genetics.chromosomes", 8, 1, 32,
                  "Number of chromosomes. Independent assortment happens per chromosome, so this "
                  "sets how much of the genome travels together.");
    c.registerInt("genetics.gene_capacity", 320, 64, 2048,
                  "Maximum genes per genome. Duplication cannot grow a genome past this ceiling.");
    c.registerInt("genetics.genes_per_trait", 4, 1, 32,
                  "Coding loci per trait. Every visible trait is polygenic by construction.");
    c.registerInt("genetics.regulatory_genes", 14, 0, 128,
                  "Genes that modulate the expression of a trait rather than contributing to it. "
                  "This is real epistasis: an allele's effect depends on a different locus.");
    c.registerInt("genetics.junk_genes", 26, 0, 256,
                  "Neutral loci with no phenotypic effect. They drift under mutation alone, which "
                  "is what makes them the right clock for phylogeny and Fst.");
    c.registerInt("genetics.mhc_genes", 6, 0, 32,
                  "Immune recognition loci. Overdominant, so heterozygotes resist more pathogens.");
    c.registerFloat("genetics.lethal_capable_fraction", 0.06, 0.0, 1.0,
                    "Fraction of coding loci that can carry a recessive lethal or sublethal allele. "
                    "This is what gives inbreeding a real, countable cost.");
    c.registerFloat("genetics.chromosome_length_cm", 100.0, 10.0, 1000.0,
                    "Genetic map length of each chromosome in centiMorgans.");
    c.registerFloat("genetics.crossover_rate", 1.4, 0.0, 10.0,
                    "Mean crossovers per chromosome per meiosis. Lower means stronger linkage.");
    c.registerInt("genetics.hotspots_per_chromosome", 3, 0, 32,
                  "Recombination hotspots per chromosome. Crossovers cluster at these.");
    c.registerFloat("genetics.founder_allele_sigma", 0.6, 0.0, 4.0,
                    "Standard deviation of founder allele values. Larger means more standing "
                    "genetic variation to start from.");
    c.registerFloat("genetics.founder_mhc_sigma", 1.8, 0.0, 8.0,
                    "Founder allele spread at MHC loci, which start deliberately diverse.");
    c.registerFloat("genetics.developmental_noise", 0.35, 0.0, 3.0,
                    "Environmental and developmental noise added at expression. This is why the "
                    "same genotype does not produce the same phenotype twice.");
    c.registerFloat("genetics.allele_bin_width", 0.25, 0.01, 2.0,
                    "Alleles are continuous, so population-genetic statistics discretise them into "
                    "bins this wide -- the natural resolution of one point mutation.");
    c.registerFloat("genetics.sex_locus_shift", 0.34, 0.0, 1.0,
                    "How far the sex locus shifts sex expression. Less than 0.5 leaves room for "
                    "intermediate outcomes.");
    c.registerFloat("genetics.ornament_metabolic_cost", 0.22, 0.0, 5.0,
                    "Energy per hour per unit of ornament. The brake on Fisherian runaway: without "
                    "a real cost, display could grow without limit.");

    // -- mutation ------------------------------------------------------------
    c.registerFloat("mutation.point_rate", 0.004, 0.0, 1.0,
                    "Per gene per meiosis chance of a Gaussian nudge to one allele.");
    c.registerFloat("mutation.point_sigma", 0.18, 0.0, 2.0,
                    "Size of a point mutation.");
    c.registerFloat("mutation.insertion_rate", 0.0006, 0.0, 1.0,
                    "Per genome chance a brand-new random gene appears.");
    c.registerFloat("mutation.deletion_rate", 0.0008, 0.0, 1.0,
                    "Per gene chance of loss. The sex locus is never deleted.");
    c.registerFloat("mutation.duplication_rate", 0.0012, 0.0, 1.0,
                    "Per gene chance of duplication. THIS is what lets genome complexity grow: "
                    "the copy is a new locus that can diverge and pick up a new function.");
    c.registerFloat("mutation.inversion_rate", 0.0004, 0.0, 1.0,
                    "Per genome chance a segment reverses, which suppresses crossover through it "
                    "and locks the alleles inside into a co-inherited block.");
    c.registerFloat("mutation.translocation_rate", 0.0003, 0.0, 1.0,
                    "Per gene chance of moving to a different chromosome.");
    c.registerFloat("mutation.chromosome_dup_rate", 0.00002, 0.0, 0.1,
                    "Per meiosis chance of whole-chromosome duplication: the origin of polyploidy.");
    c.registerFloat("mutation.lethal_rate", 0.00025, 0.0, 0.1,
                    "Per capable gene per meiosis chance an allele breaks outright.");
    c.registerFloat("mutation.regulatory_shift_rate", 0.002, 0.0, 1.0,
                    "Per regulatory gene chance of retargeting to a different trait.");

    // -- brain ---------------------------------------------------------------
    c.registerInt("brain.node_capacity", 128, 72, 512,
                  "Maximum neurons per brain. 48 inputs and 20 outputs are fixed, so the rest is "
                  "the hidden-layer ceiling.");
    c.registerInt("brain.connection_capacity", 224, 32, 4096,
                  "Maximum synapses per brain.");
    c.registerFloat("brain.founder_connection_density", 0.12, 0.0, 1.0,
                    "Fraction of possible input-to-output connections a founder starts with. Sparse "
                    "on purpose: useful structure should have to be found.");
    c.registerFloat("brain.plastic_fraction", 0.45, 0.0, 1.0,
                    "Fraction of connections subject to lifetime learning. The rest are fixed by "
                    "the genome and change only across generations.");
    c.registerFloat("brain.weight_mutation_rate", 0.06, 0.0, 1.0,
                    "Per connection per birth chance of a weight nudge.");
    c.registerFloat("brain.weight_mutation_sigma", 0.22, 0.0, 2.0,
                    "Size of a weight mutation.");
    c.registerFloat("brain.add_connection_rate", 0.05, 0.0, 1.0,
                    "Per birth chance of growing a new synapse. Cycles are allowed: they become "
                    "the recurrent edges that give the network memory.");
    c.registerFloat("brain.add_node_rate", 0.018, 0.0, 1.0,
                    "Per birth chance of splitting a connection with a new neuron. The first new "
                    "edge is weighted 1.0 so behaviour is initially almost unchanged.");
    c.registerFloat("brain.toggle_connection_rate", 0.012, 0.0, 1.0,
                    "Per connection per birth chance of being enabled or disabled.");
    c.registerFloat("brain.activation_mutation_rate", 0.01, 0.0, 1.0,
                    "Per neuron per birth chance its activation function changes.");
    c.registerFloat("brain.disjoint_inherit_chance", 0.65, 0.0, 1.0,
                    "Chance a connection present in only one parent is inherited. Below 1.0 so "
                    "topology can simplify as well as grow.");
    c.registerFloat("brain.trace_decay", 0.92, 0.0, 0.9999,
                    "Eligibility trace decay per hour. This sets how far back in a behaviour chain "
                    "a reward can still assign credit.");
    c.registerFloat("brain.value_baseline_rate", 0.02, 0.0, 1.0,
                    "How fast the reward baseline tracks actual reward. A fully predicted reward "
                    "produces no prediction error and therefore teaches nothing.");
    c.registerFloat("brain.weight_decay", 0.00002, 0.0, 0.01,
                    "Passive decay of learned weights, so unreinforced learning fades.");
    c.registerFloat("brain.aggression_bias", 2.0, -4.0, 8.0,
                    "Negative resting bias on the attack output. Without it, random weights make "
                    "unprovoked aggression fire about half the time and it kills roughly a third "
                    "of every founding population. Stored as an ordinary heritable allele, so "
                    "selection can undo it.");
    c.registerFloat("brain.sharing_bias", 1.2, -4.0, 8.0,
                    "Negative resting bias on the share output, for the same reason.");
    c.registerBool("brain.innate_reflexes", true,
                   "Give founder brains a small set of innate reflexes: drink when thirsty, eat "
                   "when hungry, turn toward what you see, flee when hurt, display when ready. "
                   "Newborn mammals root and suckle without learning. Turn this OFF to watch the "
                   "honest baseline: a purely random network dehydrates in about ten days and the "
                   "founding population goes extinct before learning or selection can start.");
    c.registerFloat("brain.innate_reflex_strength", 2.5, 0.0, 8.0,
                    "Weight given to innate reflex connections. They live in the GENOME, so "
                    "evolution can strengthen, weaken or discard every one of them.");

    // -- sex and attraction --------------------------------------------------
    c.registerInt("sex.system", 0, 0, 2,
                  "Sex determination: 0 = XY (male heterogametic), 1 = ZW (female heterogametic), "
                  "2 = temperature-dependent.");
    c.registerFloat("sex.pivot_temperature", 29.0, -20.0, 60.0,
                    "Pivot temperature for temperature-dependent sex determination.");
    c.registerFloat("sex.intersex_low", 0.42, 0.0, 0.5,
                    "Lower bound of the sex-expression band counted as ambiguous.");
    c.registerFloat("sex.intersex_high", 0.58, 0.5, 1.0,
                    "Upper bound of the ambiguous band.");
    c.registerFloat("sex.intersex_fertility", 0.35, 0.0, 1.0,
                    "Fertility multiplier when either partner's expression is ambiguous. Reduced, "
                    "not abolished.");
    c.registerFloat("attraction.base_threshold", 0.35, -5.0, 5.0,
                    "Attraction score an agent of zero selectivity requires before accepting.");
    c.registerFloat("attraction.selectivity_scale", 1.1, 0.0, 5.0,
                    "How much the PartnerSelectivity trait raises that threshold.");
    c.registerFloat("attraction.drive_relief", 0.75, 0.0, 5.0,
                    "How much accumulated reproductive drive lowers the threshold. A long-unmated "
                    "individual genuinely relaxes its standards.");
    c.registerFloat("attraction.familiarity_weight", 0.45, -3.0, 3.0,
                    "Weight on repeated harmless encounters.");
    c.registerFloat("attraction.reputation_weight", 0.35, -3.0, 3.0,
                    "Weight on the target's general standing in the group.");
    c.registerFloat("attraction.history_weight", 0.80, -3.0, 3.0,
                    "Weight on the remembered valence of past interactions with this individual.");
    c.registerFloat("attraction.bond_bonus", 1.20, -5.0, 5.0,
                    "Attraction bonus toward an existing bonded partner.");
    c.registerFloat("attraction.rejection_memory", 0.18, 0.0, 5.0,
                    "Attraction penalty after being refused. Rejection is remembered.");
    c.registerFloat("attraction.imprint_weight", 0.55, -3.0, 3.0,
                    "Weight on similarity to the template imprinted in early life.");
    c.registerFloat("attraction.imprint_chance", 0.7, 0.0, 1.0,
                    "Probability a newborn imprints on a parent at all.");
    c.registerFloat("attraction.kin_avoidance", 2.2, 0.0, 20.0,
                    "Attraction penalty per unit of relatedness. Inbreeding avoidance can evolve "
                    "because inbreeding depression here is real, not decorative.");

    // -- statistics ----------------------------------------------------------
    c.registerInt("stats.genetics_sample", 600, 20, 20000,
                  "Individuals sampled for population-genetic statistics. This is a REPORTING "
                  "cost only: every agent is still simulated in full.");
    c.registerInt("stats.fst_grid", 3, 2, 8,
                  "Grid divisions per axis used to define geographic subpopulations for Fst.");
    c.registerInt("stats.period_ticks", 720, 1, 86400,
                  "Ticks between population-genetics recomputations (default: monthly).");

    // -- god mode and rule overrides (M5) ------------------------------------
    c.registerInt("god.undo_depth", 64, 1, 4096,
                  "Maximum divine acts kept on the undo stack. Each record stores the tiles and "
                  "whole agents the act touched, so a mass kill of a thousand individuals is a "
                  "large record.");
    c.registerFloat("god.undo_memory_mb", 512.0, 8.0, 16384.0,
                    "Memory budget for undo history. Oldest records are dropped past this rather "
                    "than letting the history quietly consume the machine.", true);
    c.registerFloat("god.default_brush_radius", 8.0, 0.5, 512.0,
                    "Starting radius of terrain and resource brushes, in tiles.");
    c.registerFloat("god.default_brush_intensity", 1.0, 0.0, 10.0,
                    "Starting brush intensity. Brushes fall off smoothly to zero at the rim.");

    c.registerFloat("rules.metabolism_multiplier", 1.0, 0.0, 20.0,
                    "Global multiplier on baseline metabolic burn. Below 1 makes food go further; "
                    "0 makes agents cost nothing to keep alive.");
    c.registerFloat("rules.mutation_rate_multiplier", 1.0, 0.0, 1000.0,
                    "Global multiplier on every mutation rate, applied on top of each "
                    "individual's own heritable mutator alleles.", true);
    c.registerFloat("rules.lifespan_multiplier", 1.0, 0.01, 100.0,
                    "Scales every individual's intrinsic lifespan, and therefore the Gompertz "
                    "mortality curve.");
    c.registerFloat("rules.gestation_multiplier", 1.0, 0.01, 100.0,
                    "Speeds up or slows down gestation. Above 1 shortens pregnancies.");
    c.registerFloat("rules.learning_rate_multiplier", 1.0, 0.0, 100.0,
                    "Scales the reward signal driving lifetime learning. 0 freezes learning for "
                    "the whole population without freezing the brains themselves.");
    c.registerFloat("rules.reaction_rate_multiplier", 1.0, 0.0, 1000.0,
                    "Scales every reaction RATE. Applied to the Arrhenius rate only and never to "
                    "dG, so speeding a reaction up cannot make an uphill one spontaneous -- that "
                    "would be rewriting thermodynamics rather than adjusting kinetics.");
    c.registerFloat("rules.gravity", 9.81, 0.0, 100.0,
                    "Surface gravity in m/s2. Affects projectile and structural physics; body "
                    "mechanics are not yet gravity-dependent.");
    c.registerBool("rules.disable_aging", false,
                   "Freeze biological age at zero. Damage still accumulates and is still shown, "
                   "so you can watch what would have killed them.");
    c.registerBool("rules.disable_violence", false,
                   "Suppress the attack action entirely. Aggression still evolves in the genome; "
                   "it simply cannot be expressed.");
    c.registerBool("rules.disable_death", false,
                   "Nothing dies. An agent that would have died is held at the brink instead.");

    c.registerInt("lua.instruction_budget", 20000000, 1000, 2000000000,
                  "Maximum VM instructions one console chunk may execute. A script runs with "
                  "the world lock held, so an unbounded loop would hang the simulation; this "
                  "stops it instead.", true);


    // -- economy (M8, dormant) -----------------------------------------------
    // Every one of these does nothing at all until an economy is brought into
    // existence through god mode. They are registered so the control is honest
    // about what it would do, not because anything reads them by default.
    c.registerFloat("econ.trade_threshold", 0.55, 0.0, 1.0,
                    "Share output above which an individual is willing to exchange goods. The "
                    "prosocial output is reused rather than a new one added, because the brain's "
                    "output count is part of the genome format: adding one would invalidate every "
                    "saved brain.");
    c.registerFloat("econ.trade_range", 2.0, 0.5, 20.0,
                    "How close two individuals must be to trade, in tiles.");
    c.registerFloat("econ.min_gain", 0.15, 0.0, 10.0,
                    "How much joint surplus an exchange must produce before it happens. Both "
                    "sides must gain, or it is a donation rather than a trade.");
    c.registerFloat("econ.trade_fraction", 0.5, 0.01, 1.0,
                    "What fraction of a holding changes hands in one exchange.");
    c.registerInt("econ.detect_period_ticks", 2160, 0, 1000000,
                  "How often the money detector runs. 0 disables detection, which means a "
                  "currency can then only ever be decreed.");
    c.registerFloat("econ.moneyness_threshold", 0.55, 0.05, 1.0,
                    "How strongly a good must behave as a medium of exchange before it is called "
                    "money. Combines turnover, the rate at which takers pass it on rather than "
                    "consume it, and how reliably it is accepted.");
    c.registerInt("econ.moneyness_passes", 3, 1, 100,
                  "How many consecutive detection passes a good must hold that property. One "
                  "pass is a fluctuation; several is a fact about the economy.");
    c.registerInt("econ.money_min_trades", 200, 1, 10000000,
                  "Minimum trades in a single good before it can be considered money at all, so "
                  "a good traded twice by chance cannot win on ratios alone.");
    c.registerInt("econ.money_min_goods", 6, 2, 200,
                  "How many different goods must be circulating before money can be declared. "
                  "On a thin economy whatever is moving looks both dominant and universally "
                  "accepted, so the first good to cross the line locks in on an artefact -- and "
                  "since detection stops once a currency exists, that mistake is permanent.");
    c.registerInt("econ.money_min_total_trades", 2000, 1, 100000000,
                  "Total trades across all goods before money can be declared, for the same "
                  "reason: three factors measured on a handful of exchanges are noise.");

    // -- speciation ----------------------------------------------------------
    c.registerInt("species.period_ticks", 2160, 0, 1000000,
                  "How often the species detector runs, in ticks. This is a MEASUREMENT of the "
                  "population rather than a process acting on it, so running it more often than "
                  "divergence can change would be pure cost. 0 disables detection entirely.");
    c.registerFloat("species.gap_factor", 4.0, 1.1, 40.0,
                    "How wide a gap counts as a species boundary, as a multiple of the "
                    "population's OWN median nearest-neighbour distance. Relative to that "
                    "spacing rather than absolute, because a species boundary is a "
                    "discontinuity and not a distance: members of one population always have a "
                    "close relative, and members of a reproductively isolated one do not. "
                    "Lower splits hairs; higher lumps distinct populations together.");
    c.registerInt("species.min_founders", 8, 1, 10000,
                  "How many mutually similar individuals it takes to name a new lineage. A "
                  "cluster of two is a pair of odd siblings, not a species.");
    c.registerInt("species.cluster_sample", 400, 20, 4000,
                  "Cap on how many individuals the clustering pass compares. Clustering is "
                  "quadratic, so it is bounded -- and like the population-genetics window this "
                  "is a REPORTING sample, never a simulation shortcut: every agent is still "
                  "simulated in full and every agent is still assigned to a lineage.", true);
    c.registerInt("species.extinction_grace_ticks", 8640, 0, 10000000,
                  "How long a lineage may have no developed members before it is declared "
                  "extinct. Without a grace period a population whose adults all happened to be "
                  "pregnant would be buried and then resurrected under a new name.");
    c.registerFloat("species.hybrid_penalty", 1.0, 0.0, 1.0,
                    "Strength of reproductive isolation between diverged lineages. 0 makes a "
                    "species a pure label with no consequence; 1 makes a fully diverged cross "
                    "sterile. Applied from the pair's neutral distance, never from their labels, "
                    "so it stays continuous.");
    c.registerFloat("species.hybrid_onset", 4.0, 0.0, 40.0,
                    "Neutral distance, in units of the population's own spacing, below which a "
                    "cross pays no penalty at all. Keeps ordinary within-population pairs "
                    "completely unaffected.");
    c.registerFloat("species.hybrid_full", 12.0, 0.2, 80.0,
                    "Distance, in the same units, at which the penalty reaches full strength. "
                    "Between the onset and here it grows as the square of the excess, because "
                    "hybrid breakdown compounds: the further apart the parents, the more "
                    "independent incompatibilities a zygote has to survive at once.");

    // -- chemistry, discovery and culture ------------------------------------
    c.registerInt("chem.inventory_slots", 8, 2, 32,
                  "How many distinct substances an agent can carry. Small on purpose: an "
                  "individual holding forty reagents is not a forager.", true);
    c.registerFloat("chem.gather_amount", 0.5, 0.01, 20.0,
                    "Moles of substance taken from a tile in one gathering action.");
    c.registerFloat("chem.gather_threshold", 0.5, 0.0, 1.0,
                    "Pick-up output above which an agent takes what is underfoot.");
    c.registerFloat("chem.experiment_threshold", 0.55, 0.0, 1.0,
                    "Use output above which an agent brings its held substances together and "
                    "finds out what happens.");
    c.registerFloat("chem.experiment_curiosity", 0.25, 0.0, 1.0,
                    "Accumulated curiosity drive an agent needs before it will run an "
                    "experiment. Curiosity accrues slowly and an experiment discharges it, so "
                    "this sets how often experimenting is even possible. Lowering it toward "
                    "zero makes discovery cheap and fast, and much less interesting.");
    c.registerFloat("chem.experiment_energy_cost", 4.0, 0.0, 200.0,
                    "Energy an experiment costs. Exploration has to be paid for, or it is not "
                    "a trade-off against foraging.");
    c.registerFloat("chem.experiment_amount", 1.0, 0.01, 100.0,
                    "Scale of one experiment, as a multiple of the stoichiometric coefficients.");
    c.registerFloat("chem.novelty_reward", 2.0, 0.0, 20.0,
                    "Reward pulse for an outcome the agent has never seen. Paid for NOVELTY, "
                    "not for usefulness -- which is what makes exploring worth doing before you "
                    "know what it will turn up.");
    c.registerFloat("chem.teach_threshold", 0.55, 0.0, 1.0,
                    "Teach output above which an agent passes knowledge to a neighbour.");
    c.registerFloat("chem.teach_range", 2.0, 0.5, 20.0,
                    "How far teaching carries, in tiles.");
    c.registerInt("knowledge.capacity", 64, 4, 4096,
                  "How many knowledge units one agent can hold. When full, the least valued is "
                  "displaced -- which is why useless knowledge is lost first.", true);
    c.registerFloat("knowledge.teach_fidelity_loss", 0.12, 0.0, 1.0,
                    "Mean fidelity lost each time knowledge is passed on. Below 0.35 fidelity a "
                    "unit stops working, so a long enough chain of retelling destroys it.");
    c.registerFloat("knowledge.teach_fidelity_noise", 0.1, 0.0, 1.0,
                    "Random spread on that loss. Some tellings are much worse than others.");
    c.registerFloat("knowledge.technique_transmission", 0.35, 0.0, 1.0,
                    "Chance that a technique the teacher has and the student lacks crosses over "
                    "as well. Techniques spread faster than the knowledge that uses them.");
    c.registerFloat("knowledge.technique_discovery_rate", 0.00002, 0.0, 1.0,
                    "Per-tick chance of independently working out the next technique up the "
                    "ladder. Deliberately tiny: fire should be a rare event, not a schedule.");
    c.registerFloat("knowledge.valuation_decay", 0.999, 0.5, 1.0,
                    "Per-use decay on how much a holder values a piece of knowledge.");

    // -- rendering -----------------------------------------------------------
    c.registerBool("render.vsync", true,
                   "Synchronise presentation to the display refresh. Off uncaps render FPS.");
    c.registerFloat("render.zoom_speed", 1.14, 1.01, 2.0,
                    "Multiplier applied to zoom per mouse wheel notch.");
    c.registerFloat("render.min_pixels_per_tile", 0.05, 0.005, 4.0,
                    "Furthest zoom out, in screen pixels per world tile.", true);
    c.registerFloat("render.max_pixels_per_tile", 64.0, 1.0, 512.0,
                    "Closest zoom in, in screen pixels per world tile.");
    c.registerBool("render.show_grid", false,
                   "Draw tile gridlines when zoomed in far enough for them to be legible.");
    c.registerFloat("render.ui_scale", 1.0, 0.5, 3.0,
                    "Global scale factor for all interface fonts and widgets.");
}

}  // namespace gen
