# GENESIS

A high-fidelity artificial-life simulator. Evolved creatures with real genetics
and small neural-network brains are meant to live, learn, reproduce, form
societies, discover real chemistry and build their own technology, while you
observe and intervene as an omnipotent god through a proper graphical interface.

**This repository currently contains milestones M1–M5.** M1 is the world:
geology, hydrology, climate, ecology, the deterministic time engine and the UI
shell. M2–M4 add the inhabitants: a real diploid genome, genome-encoded neural
brains with lifetime learning, continuous sex expression, and relational
attraction. M5 adds god mode: brushes, disasters, population tools, undo/redo, miracles and an embedded Lua console. Read [Population status](#population-status-read-this) for an
honest account of what those creatures currently do and do not manage. See
[Milestone status](#milestone-status) for exactly what exists, what is partial,
and what has not been started — the same table is available in the application
under *Help → Milestone status*, so the program never overstates itself either.

---

## Build

### Requirements

- Windows 10/11 x64
- Either MinGW-w64 / TDM-GCC (tested: TDM-GCC 10.3.0) or MSVC 2019+
- `git`, to fetch the two vendored dependencies

There is nothing else to install. No SDL, no GLFW, no Qt, no Boost, no GL
loader, no `.lib` files to hunt down. `Genesis.exe` links only against OS DLLs
(`opengl32`, `gdi32`, `user32`, `shell32`, `dwmapi`, `imm32`) plus a statically
linked C++ runtime, so it is a single-file, double-click, no-admin executable.

### One-time: fetch the vendored sources

```bash
git clone --depth 1 -b docking https://github.com/ocornut/imgui.git vendor/imgui
```

```bash
git clone --depth 1 -b v5.4.7 https://github.com/lua/lua.git vendor/lua
```

Both are compiled from source into the same executable. There is still nothing
to install and nothing to ship alongside `Genesis.exe`.

### MinGW-w64 — the simple path

```bash
build.bat
```

`build.bat debug` builds with symbols; `build.bat clean` removes artefacts.

### MinGW-w64 — one line

Lua is C and everything else is C++, so the Lua sources carry an explicit `-x c`
and the rest an explicit `-x c++`. GCC's driver applies `-x` to the single file
that follows it, which is why it appears once per Lua file.

```bash
g++ -std=c++17 -O2 -m64 -Wall -Wextra -fno-fast-math -Isrc -Ivendor/imgui -Ivendor/imgui/backends -Ivendor/lua -DWIN32_LEAN_AND_MEAN -DNOMINMAX -x c vendor/lua/lapi.c -x c vendor/lua/lauxlib.c -x c vendor/lua/lbaselib.c -x c vendor/lua/lcode.c -x c vendor/lua/lcorolib.c -x c vendor/lua/lctype.c -x c vendor/lua/ldblib.c -x c vendor/lua/ldebug.c -x c vendor/lua/ldo.c -x c vendor/lua/ldump.c -x c vendor/lua/lfunc.c -x c vendor/lua/lgc.c -x c vendor/lua/linit.c -x c vendor/lua/liolib.c -x c vendor/lua/llex.c -x c vendor/lua/lmathlib.c -x c vendor/lua/lmem.c -x c vendor/lua/loadlib.c -x c vendor/lua/lobject.c -x c vendor/lua/lopcodes.c -x c vendor/lua/loslib.c -x c vendor/lua/lparser.c -x c vendor/lua/lstate.c -x c vendor/lua/lstring.c -x c vendor/lua/lstrlib.c -x c vendor/lua/ltable.c -x c vendor/lua/ltablib.c -x c vendor/lua/ltm.c -x c vendor/lua/lundump.c -x c vendor/lua/lutf8lib.c -x c vendor/lua/lvm.c -x c vendor/lua/lzio.c -x c++ src/main.cpp src/core/config.cpp src/core/noise.cpp src/core/json.cpp src/core/profiler.cpp src/core/serialize.cpp src/sim/time.cpp src/sim/world.cpp src/sim/worldgen.cpp src/sim/genetics.cpp src/sim/brain.cpp src/sim/attraction.cpp src/sim/agent.cpp src/sim/simulation.cpp src/chem/elements.cpp src/chem/reactions.cpp src/chem/materials.cpp src/sim/knowledge.cpp src/sim/species.cpp src/sim/chemistry_agent.cpp src/econ/economy.cpp src/god/god.cpp src/god/lua_api.cpp src/ui/app.cpp src/ui/panels.cpp src/ui/viewport.cpp src/ui/cards.cpp src/ui/chem_ui.cpp src/ui/profiler_ui.cpp src/ui/phylogeny_ui.cpp src/ui/econ_ui.cpp src/ui/god_ui.cpp vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp vendor/imgui/imgui_demo.cpp vendor/imgui/backends/imgui_impl_win32.cpp vendor/imgui/backends/imgui_impl_opengl2.cpp -o Genesis.exe -mwindows -s -static -static-libgcc -static-libstdc++ -lopengl32 -lgdi32 -luser32 -lshell32 -ldwmapi -limm32
```

This emits 32 copies of one benign driver warning — `command-line option
'-std=c++17' is valid for C++/ObjC++ but not for C` — because a single
invocation cannot carry a different `-std` per language. It is a complaint about
a flag, not about any code: nothing in `src/` and nothing in Lua produces a
warning under `-Wall -Wextra`. `build.bat` compiles the two languages separately
and is therefore completely silent.

### MSVC — one line

From a *x64 Native Tools Command Prompt*. `cl` picks the language from the file
extension, so no per-file switch is needed:

```bash
cl /std:c++17 /O2 /W4 /permissive- /EHsc /MT /fp:precise /MP /Isrc /Ivendor\imgui /Ivendor\imgui\backends /Ivendor\lua /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS vendor\lua\lapi.c vendor\lua\lauxlib.c vendor\lua\lbaselib.c vendor\lua\lcode.c vendor\lua\lcorolib.c vendor\lua\lctype.c vendor\lua\ldblib.c vendor\lua\ldebug.c vendor\lua\ldo.c vendor\lua\ldump.c vendor\lua\lfunc.c vendor\lua\lgc.c vendor\lua\linit.c vendor\lua\liolib.c vendor\lua\llex.c vendor\lua\lmathlib.c vendor\lua\lmem.c vendor\lua\loadlib.c vendor\lua\lobject.c vendor\lua\lopcodes.c vendor\lua\loslib.c vendor\lua\lparser.c vendor\lua\lstate.c vendor\lua\lstring.c vendor\lua\lstrlib.c vendor\lua\ltable.c vendor\lua\ltablib.c vendor\lua\ltm.c vendor\lua\lundump.c vendor\lua\lutf8lib.c vendor\lua\lvm.c vendor\lua\lzio.c src\main.cpp src\core\config.cpp src\core\noise.cpp src\core\json.cpp src\core\profiler.cpp src\core\serialize.cpp src\sim\time.cpp src\sim\world.cpp src\sim\worldgen.cpp src\sim\genetics.cpp src\sim\brain.cpp src\sim\attraction.cpp src\sim\agent.cpp src\sim\simulation.cpp src\chem\elements.cpp src\chem\reactions.cpp src\chem\materials.cpp src\sim\knowledge.cpp src\sim\species.cpp src\sim\chemistry_agent.cpp src\econ\economy.cpp src\god\god.cpp src\god\lua_api.cpp src\ui\app.cpp src\ui\panels.cpp src\ui\viewport.cpp src\ui\cards.cpp src\ui\chem_ui.cpp src\ui\profiler_ui.cpp src\ui\phylogeny_ui.cpp src\ui\econ_ui.cpp src\ui\god_ui.cpp vendor\imgui\imgui.cpp vendor\imgui\imgui_draw.cpp vendor\imgui\imgui_tables.cpp vendor\imgui\imgui_widgets.cpp vendor\imgui\imgui_demo.cpp vendor\imgui\backends\imgui_impl_win32.cpp vendor\imgui\backends\imgui_impl_opengl2.cpp /link /SUBSYSTEM:WINDOWS /OUT:Genesis.exe opengl32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib imm32.lib
```

### CMake — either compiler

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release
```

The build is clean at `-Wall -Wextra` (and `/W4`) with **zero warnings**.

> **Verified on this machine:** TDM-GCC 10.3.0 x86_64-w64-mingw32, `build.bat`,
> clean rebuild, 0 warnings, 0 errors, `Genesis.exe` = 2.54 MB, 25,998 lines of
> project source plus vendored Dear ImGui and Lua 5.4.7. The single-command
> MinGW form above was also built and run from a clean copy of the tree.
> The MSVC and CMake paths are written to the same source set but were not
> exercised here — there is no MSVC toolchain on this machine.

---

## Running

```bash
Genesis.exe
```

That is all it needs. A missing `data/config.ini` is not an error: the registry
defaults are identical to the shipped file's values, so a bare `Genesis.exe` in
an empty folder generates the same world.

### Headless batch mode

No window, no GL, no UI. Leave a long run going and open the result later.

```bash
Genesis.exe --headless --years 500 --seed 42 --snapshot run.gen --csv run.csv
```

| Flag | Meaning |
| --- | --- |
| `--headless` | Run with no window at all |
| `--years N` | Simulated years to run (default 100) |
| `--seed N` | World seed (default 20260728) |
| `--width N` `--height N` | World size in tiles (default from config) |
| `--load PATH` | Resume from a snapshot; `--years` then means N years *more* |
| `--snapshot PATH` | Where to write the final world snapshot |
| `--csv PATH` | Where to write the telemetry time series |
| `--quiet` | Suppress per-year progress output |
| `--trace-agent N` | Print one agent's state, plus a population diagnostic, every N ticks |
| `--script PATH` | Run a Lua chunk once after the world is built. This is how god mode is driven without a GUI |
| `--config PATH` | Config file to load (default `data/config.ini`) |
| `--write-config` | Write a fully commented config file and exit |
| `--help` | Usage |

### Controls

| Input | Action |
| --- | --- |
| Left drag / middle drag | Pan |
| Mouse wheel | Zoom about the cursor |
| Left click | Select a tile |
| Arrow keys | Pan |
| `+` / `-` | Zoom |
| `Space` | Pause / resume |
| `1`–`7` | Speed presets 1× 2× 5× 10× 25× 50× 100× |
| `M` | MAX speed (uncapped) |
| `.` | Advance exactly one tick |
| `G` | Tile grid |
| `H` | Hide render (spend the frame budget on ticks) |
| `Ctrl+S` / `Ctrl+O` | Save / load snapshot |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo the last divine act |
| `Ctrl+0`..`Ctrl+9` | Cast the miracle bound to that key |
| `F6` | Phylogeny (species tree and individual lineage) |
| `F7` | Chemistry Lab |
| `F8` | Knowledge & Culture |
| `F9` | Profiler (where the tick actually goes) |
| `Esc` | Put the brush down |

Panels are dockable and the layout is saved to `genesis_layout.ini`. *View →
Reset layout* restores the default arrangement.

---

## Performance, honestly

Measured on this machine (15 worker threads + the calling thread), release
build, no agents:

| World | Generation | Throughput at MAX | In sim-years |
| --- | --- | --- | --- |
| 384 × 384 | 0.03 s | ~10,000 ticks/s | 1.16 yr/s |
| 512 × 512 | 0.05 s | ~7,300 ticks/s | 0.84 yr/s |
| 1024 × 1024 | 0.16 s | ~2,340 ticks/s | 0.27 yr/s |

One tick is one simulated hour, and 1× is defined as one tick per real second,
so "2,340 ticks/s" *is* 2,340×. The UI reports this measured figure rather than
the requested one, which is the point: **MAX is uncapped and the ceiling is
whatever the CPU actually delivers.**

Memory is ≈64 bytes per tile: **67 MB at 1024², about 1.07 GB at 4096².** The
4096² ceiling is supported but is a 16 GB-RAM proposition; 1024² is the default
and the tested configuration.

### Why fast-forward is not an approximation

Fast-forward is achieved purely by executing more real ticks per second. Nothing
that happens at 100× is a statistical stand-in for what would have happened at
1×. When agents exist, every agent's brain will be evaluated on every tick at
every speed, with no cohorts, no LOD and no distant-region approximation.

The environment is treated differently, and the difference is physical rather
than budgetary. Glacial advance and sediment deposition have no meaningful
hour-to-hour dynamics; integrating them hourly would add numerical noise, not
fidelity. So environmental processes are integrated by operator splitting at
their natural timescale:

| Process | Cadence | Config key |
| --- | --- | --- |
| Insolation, day/night, air temperature | every tick | — |
| Tile thermal relaxation | 6 h | `env.thermal_period_ticks` |
| Evaporation, soil moisture, plant growth | 1 day | `env.hydrology_period_ticks`, `env.ecology_period_ticks` |
| Rainfall pattern, flow accumulation | 1 month | `env.weather_period_ticks` |
| Erosion, sedimentation, glaciation | 1 year | `env.geology_period_ticks` |

Every cadence is editable and can be set to 1 to integrate everything hourly if
you want to pay for it. The time bar's *Bottleneck* readout names the stage that
dominated the last tick, so the cost of changing them is visible.

---

## Determinism

**One 64-bit seed reproduces a run bit-for-bit** on the same binary.

Verified here, not merely claimed:

- Two runs, same seed, 2 simulated years, 384² world → **entire snapshot file
  byte-identical**.
- A different seed → different world, as expected.
- Run 4 years continuously vs. run 2 years, save, resume, run 2 more → **world
  state byte-identical**, and the telemetry agrees to the last significant digit.

How it is preserved:

- **Per-subsystem PRNG streams.** xoshiro256++ seeded through SplitMix64, one
  independent stream per subsystem. Consuming extra randomness in one subsystem —
  which is exactly what a god-mode intervention does — cannot shift the sequence
  any other subsystem sees. Spawning agents will not perturb the weather.
- **Deterministic parallelism.** `parallelFor` always splits `[0,n)` into
  `workerCount+1` contiguous chunks by the same integer arithmetic, regardless of
  load or timing. No work stealing, no dynamic scheduling.
- **No floating-point atomics.** Float addition is not associative, so every
  reduction accumulates into a per-chunk slot and sums them in ascending chunk
  order. Worker count is stored in the save file and restored on load, because
  changing the split would change the summation order.
- **No `-ffast-math` / `/fp:fast`.** Both are explicitly disabled in every build
  path; they reassociate operations and would break reproducibility.
- **No wall-clock reads in simulation logic.** Real time influences how many
  ticks run, never what a tick does. The single exception is picking a random
  seed when you tick "Random seed" in the regenerate dialog.
- **No struct blitting in the file format.** Every field is written
  individually, so alignment padding never reaches disk and a snapshot is
  portable between compilers.

The one honest caveat: `sin`, `exp` and `pow` are implemented differently by
different libm versions, so bit-exactness is guaranteed *for a given binary*.
The compiler and version are recorded in the snapshot header, and loading a file
written by a different toolchain logs a warning rather than diverging silently.

---

## The models, and what they are based on

### Tectonics and topography

Plates are Voronoi domains with velocity vectors. For each tile the two nearest
plate centres are found; the difference of the distances approximates the
distance to their shared boundary, and the relative velocity along the line
joining them gives convergence. Convergent boundaries build mountains with an
exponential decay away from the fault; divergent boundaries open rifts.
Oceanic plates sit at a low baseline and continental plates high, because
continental crust is less dense and floats higher on the mantle (isostasy).

Relief comes from domain-warped fBm for the continental outline plus ridged
multifractal noise for fine detail — ridged noise turns the zero crossings of
Perlin noise into sharp crests, which is what makes ranges look like ranges.

**Sea level is a datum, not a fudge.** The requested land fraction is hit by
finding the matching elevation quantile and then *shifting the entire field* so
that quantile lands on the configured sea level. `elevation` therefore means
literally "metres relative to sea level", which is what the lapse rate, the
biome thresholds and the tile inspector all assume. Raising `world.sea_level`
afterwards floods the world instead of silently redefining the datum.

### Erosion and drainage

Thermal erosion slumps material that exceeds the angle of repose toward the
local mean. Fluvial incision follows the stream-power law — incision goes as
discharge^m · slope^n with the usual m ≈ 0.5, n ≈ 1 — with discharge proxied by
flow accumulation.

Drainage is D8 steepest descent with diagonals distance-corrected by 1/√2 so the
choice is a true gradient. Flow accumulation is then propagated in order of
descending elevation, so every tile's upstream contribution is complete before
it passes anything downstream. The ordering uses an 8192-bucket counting sort:
O(n), and deterministic because ties resolve by ascending tile index. Tiles
carrying enough discharge become channels; interior sinks that collect discharge
become lakes.

### Climate

Temperature has three terms. The mean annual profile is

&nbsp;&nbsp;&nbsp;&nbsp;`T(lat) = base + gradient · (1/3 − sin²lat)`

The area average of sin²(latitude) over a sphere is exactly 1/3, so this makes
the global area mean come out at `climate.base_temperature` *by construction*,
with `climate.pole_gradient` as the equator-to-pole contrast. At the defaults
that is +27 °C at the equator against −13 °C at the pole, on a 14 °C mean —
Earth's actual numbers.

The seasonal term is zero at the equator, maximal at the poles, and opposite in
the two hemispheres because it scales with sin(latitude). The diurnal term comes
from real solar geometry: declination swings ±`climate.axial_tilt` over the year,
the hour angle runs ±π over the day, and the cosine of the solar zenith angle is
`sin φ sin δ + cos φ cos δ cos h`, clamped at zero for night.

Tiles then relax toward that equilibrium and conduct laterally. Water relaxes
more slowly than rock — it has roughly four times the volumetric heat capacity —
which is what makes coasts mild and continental interiors extreme.

Precipitation combines latitude bands for the general circulation (wet ITCZ,
dry subtropical high near 30°, wet polar front near 60°, dry poles) with a
moisture-advection sweep along the prevailing wind. Air picks up moisture over
water at a rate that rises with temperature, and orographic lift wrings it out
in proportion to the rate of ascent, giving genuine rain shadows on lee slopes.

*Known simplification:* the advection sweep is quantised to the dominant axis of
the wind vector, so rain shadows form perpendicular to that axis rather than
exactly along the wind. The effect is correct in kind and slightly wrong in
angle for winds far from the cardinal directions.

### Geology and ore

Six strata are stored per tile, surface-first, with real rock types. Which rocks
are present follows the tectonic setting and depositional environment: thin
sediment over basalt on oceanic crust, alluvium over a sedimentary pile in river
valleys, stripped metamorphic and intrusive rock in uplifted cores, sedimentary
cover on basement in continental interiors. Coal appears where an ancient swamp
plausibly sat — low, wet, and buried.

**Ore is emplaced by process, not sprinkled.** This is what makes prospecting a
real skill later rather than a lottery:

| Deposit | Process |
| --- | --- |
| Cassiterite, wolframite | Hydrothermal veins in granite along plate-boundary faults |
| Chalcopyrite | Same, but in mafic (basaltic) host rock |
| Galena, sphalerite | Same, in sedimentary host rock |
| Malachite, native copper | Oxidised weathering cap over a sulfide body — shallow, above the water table, in a wet enough climate. This is the ore that gets smelted first in every real technological sequence, because it reduces at a temperature a wood fire can reach. |
| Hematite, magnetite | Banded iron: chemical sediment in shallow marine basins, later uplifted. Abundant, and useless until you can hit 1200 °C. |
| Native gold, stream tin | Placers: dense grains eroded out of upstream veins and dropped where the current slackens. Needs alluvium and real discharge, which is why they sit in river beds. |
| Rock salt, niter | Evaporites: an arid basin with no outlet concentrates brine until halite precipitates. |
| Native sulfur | Volcanic, near active boundaries in basaltic terrain. |
| Bauxite | Intense tropical leaching strips everything but aluminium oxide. Common, and worthless for millennia. |
| Clay | Floodplains and lake margins — the pottery and refractory feedstock, and therefore the gate to kilns. |

Each ore carries its real formula (`Fe2O3`, `Cu2CO3(OH)2`, `SnO2`, …) so the M6
chemistry engine reduces the actual mineral and the mineralogy cannot drift away
from the chemistry.

### Soil and ecology

Soil nutrients (N, P, K separately, since N comes from biological fixation and
P and K from mineral weathering) are set by parent rock and climate: basalt and
alluvium weather rich, granite poor, heavy rainfall leaches, aridity starves.

Plant growth is logistic against a per-tile carrying capacity and limited by
**Liebig's law of the minimum** — the scarcest of light, water, nutrients and
temperature sets the rate, not their product. Temperature response is Gaussian
about an optimum. Senescence returns matter to the soil, and a seed-rain floor
lets an emptied tile recolonise rather than staying dead forever.

### Biomes

Whittaker classification from mean temperature and annual rainfall, with water,
ice, wetland and alpine handled separately.

---

## Configuration

Every tunable constant is registered exactly once in `src/core/config.cpp` with
a key, description, default and range, and is read through that registry.
Nothing is hardcoded in logic. Two things follow:

1. `data/config.ini` round-trips, with each description written as a comment.
   Regenerate it any time with `Genesis.exe --write-config`.
2. **The Settings screen is generated** by walking the registry. A constant
   added by a later milestone appears automatically with its own widget,
   tooltip, range and working "restore defaults" — with no UI code written for
   it. Values differing from their default are highlighted.

---

## What to look for

Right now, in M1, with no creatures yet:

- **Rain shadows.** Switch to the Rainfall overlay and find a mountain range
  running across the prevailing wind. The windward flank is soaked and the lee
  is starved. Then check the Biome overlay over the same range: desert on one
  side, forest on the other.
- **Drainage networks.** The Drainage overlay is on a log scale because flow
  accumulation spans five orders of magnitude. Watch dendritic networks converge
  into trunk rivers and find the endorheic basins that drain nowhere.
- **Ore that makes geological sense.** Put the Ore overlay next to Surface rock.
  Placer gold sits in alluvium *downstream* of the granite that shed it. Evaporites
  sit only in arid sinks. Bauxite only in the wet tropics.
- **Seasons doing real work.** Set 100× and watch the Temperature overlay: the
  ice line advances and retreats, and the hemispheres are half a year out of
  phase. The Charts panel shows ice fraction and total biomass oscillating.
- **The cost of fidelity.** Turn on MAX, then open Settings → env and set
  `ecology_period_ticks` to 1. Watch the throughput readout and the Bottleneck
  line change. Nothing is hidden.

Once creatures exist (M2 onward), this section will grow to cover first fire,
first alloy, first war, speciation across a mountain range, and sexual-selection
runaway.

---

## Population status (read this)

M2–M4 are implemented and every mechanism demonstrably runs. What they do **not**
yet do is sustain themselves: a founding population peaks in its first year and
then declines to extinction, typically within 10–60 simulated years. That is the
honest state, and the instrumentation to see it is built in rather than hidden.

Measured on the default 1024² world, seed 42, 20 founders:

| | |
| --- | --- |
| Peak population | 17 (year 1) |
| Offspring born | 14 beyond the 20 founders |
| Extinction | year 10 |
| Deaths | starvation 29, old age 5 |
| Courtship funnel | 709 attempted → 187 mutual → 59 same-gamete → 14 conceptions |

The courtship funnel is healthy — 26% mutual acceptance, same-gamete pairings
correctly producing bonds but no zygote, conceptions following. Genetics,
inheritance, brains, learning, sex expression and attraction all behave. The gap
is demographic: individuals do not average enough surviving offspring to replace
themselves, and starvation is what removes them.

Every headless run prints this summary automatically. `--trace-agent N` dumps one
individual's inner state every N ticks plus a population-wide line (how many are
hungry, how many are standing on a bare tile, mean tile biomass, mean eat
output), which is where these numbers came from.

**Bugs found and fixed along the way**, each individually fatal:

- Founders were scattered into the ocean, where movement is refused, and starved
  in place unable to walk out.
- A blocked move stopped an agent dead instead of sliding along the obstacle, so
  its own water-seeking reflex pinned it against a shoreline until it had eaten
  its single tile down to nothing.
- Zero energy drove movement vigour to zero, making any shortfall unrecoverable:
  no energy meant no movement meant no food.
- An agent at 95% reserves still stripped a full ration from the tile and the
  surplus was clamped away — forage destroyed for nothing.
- Nutrient uptake was scaled off the growth *rate* rather than the growth *mass*,
  so soil stripped itself in a season and the food web collapsed.
- Courtship was attempted every tick, so every pair was refused within hours and
  the permanent rejection memory locked them out for good: mutual acceptance was
  0.58%. A refractory period took it to 26%.
- Unprovoked aggression fired about half the time from random weights and killed
  roughly a third of every founding population.
- `--write-config` round-tripped the existing file instead of emitting registry
  defaults, so changed defaults silently had no effect at runtime.

**What is most likely still wrong** is the balance between foraging competence
and reproductive rate, not any single mechanism. The knobs that matter most are
`ecology.plant_growth_rate`, `agents.eat_rate_kg`, `agents.base_metabolic_burn`,
`sim.founder_count`, `agents.courtship_range` and
`agents.base_conception_chance`. All are live in *View → Settings*.

---

## The inhabitants (M2–M4)

### Genetics

Diploid, **variable length**, 8 chromosomes by default. Each gene carries two
alleles plus its own identity, chromosome, map position, dominance model and
target — metadata per gene rather than in a shared schema, because that is what
makes gene duplication real: a duplicate is a genuinely new locus that can
diverge, which is the only honest way for genome complexity to grow.

- **Locus types:** coding, regulatory, junk, MHC, sex-determining.
- **Dominance** is drawn per locus: additive, complete dominant, complete
  recessive, codominant, overdominant. One trait can have all five contributing.
- **Every trait is polygenic** — 4 coding loci each by default, with skewed
  effect sizes so a few loci matter far more than the rest.
- **Regulatory genes multiply** a trait's expression, which is genuine epistasis:
  an allele's effect depends on the genotype at an unlinked locus.
- **Recombination** uses a per-chromosome map with hotspots. Linkage is not
  asserted; it falls out of crossover position and is *measured back* as linkage
  disequilibrium in the statistics.
- **Mutation:** point, insertion, deletion, duplication, inversion (which
  suppresses local crossover and locks alleles into a co-inherited block),
  translocation, whole-chromosome duplication, and outright allele breakage.
  Mutation rate is itself a heritable trait, so evolvability evolves.
- **Lethal and sublethal recessives** express at birth, giving inbreeding a real
  countable cost. Inbreeding also degrades developmental symmetry — which is
  exactly why symmetry is an honest signal rather than an arbitrary label.
- Alleles are continuous, so the population-genetic statistics (Ho, He, F,
  Tajima's D, Ne, Fst, LD) discretise them into bins of
  `genetics.allele_bin_width`, the natural resolution of one point mutation.

### Brains

48 sensory inputs → evolvable recurrent topology → 20 motor outputs, encoded in
the genome and inherited by NEAT-style alignment on innovation numbers.

Connection **weights are diploid** like everything else, combined under a
dominance model. Connection **presence** is inherited by innovation alignment
instead, because there is no meaningful dominance relation between "this
connection exists" and "it does not".

Evaluation is depth-ordered with recurrent edges reading the previous tick, so
the network has real memory without a reaction-time delay through its depth. A
CSR adjacency index built at development time makes a forward pass
O(connections) rather than O(nodes × connections) — the difference between
affordable and impossible at 10k agents.

**Lifetime learning** is a dopamine-analog reward-prediction error with Hebbian
eligibility traces on plastic connections. Reward minus *expectation* drives
learning, so a fully predicted reward teaches nothing. Because traces stay warm
for several ticks, one terminal reward reinforces the whole preceding behaviour
chain rather than only the last instant.

**Innate reflexes** (`brain.innate_reflexes`, on by default) give founders a
small set of genetically-encoded starting behaviours: drink when thirsty, eat
when hungry, turn toward what you can see, home on remembered resources, flee
when hurt, display when ready. Newborn mammals root and suckle without learning.
Turn the option off to watch the honest baseline — a purely random network
dehydrates in about ten days and the founding population is gone before
selection can start. The weights live in the genome, so evolution can strengthen,
weaken or discard every one of them.

### Sex, orientation and attraction

Chromosomal sex and **expressed** sex are stored separately and can disagree.
Expression is continuous, driven by the sex locus, hormone-analog regulatory
genes and developmental noise, so intermediate outcomes occur at low rate without
being a special case. XY, ZW and temperature-dependent determination are all
world options.

Orientation is a **2D preference space**, not a binary: independent attraction
strengths toward male-expressed and female-expressed phenotypes, so any point in
that space is representable. It is heritable but noisy, and it does not have to
be reproductively optimal.

**Attractiveness is a function of an ordered pair, not a number an individual
has.** The same target can be a 9/10 to one observer and a 2/10 to another:

```
attractiveness(A -> B) = dot(A.preferenceWeights, B.displayedTraits)
                       + A's evolved taste for MHC dissimilarity x |mhc(A) - mhc(B)|
                       + homophily + familiarity + reputation + relationship history
                       + similarity to A's early-life imprint
                       - kin-avoidance penalty
                       , all gated multiplicatively by A's orientation
```

The Attraction tab shows this decomposition term by term for every nearby
individual, in **both directions**, and lets you force or destroy any specific
pairing. Mate choice requires **mutual** acceptance against each party's own
threshold; rejection is a normal outcome and is remembered.

Ornaments carry a real metabolic surcharge — the brake on Fisherian runaway,
since without a cost display could grow without limit. The charts track mean
ornament and mean ornament *preference* together, which is what runaway looks
like from the outside.

### Life cycle

Embryo → juvenile → adolescent → adult → senescent. A zygote is created at
fertilisation and gestates as an Embryo, which is what lets a father die before
the birth without the child losing half its genome. Mortality is a Gompertz
hazard — lifespan is a distribution, not a cap — driven by accumulating cellular
damage and a telomere-analog counter. Paternity is contested by sperm competition
among recent mates, weighted by condition. Corpses return their nutrients to the
soil, closing the cycle.

---

## God mode (M5)

Every divine power is a `GodAction`: a plain value that can be logged, replayed,
stored in a miracle, and undone. Actions are applied **on the sim thread at a
tick boundary with the world lock held exclusively**, so no intervention can
tear state or race a tick, and every one lands in the Intervention Log.

### Undo and redo

Undo is **delta-based, not snapshot-based**. Each record stores the tiles and
the whole agents an action touched, both before and after — which is what lets
undo and redo be the same machinery walked in opposite directions.

Redo restores the recorded *after* state rather than re-running the action.
That distinction matters: re-running would consume randomness and produce a
different world, which is not what "redo" means.

An undone mass kill brings every individual back **whole** — genome, brain,
learned weights, position, condition, relationships and pedigree links — because
the undo record holds a full serialisation of each one, and the uid is restored
with it so every reference still resolves. Verified: kill all 20 founders, undo,
and the sample individual comes back with identical name, position, energy, Size
and Ornament1, and the same mother uid.

The cost is honest: a mass kill of a thousand agents stores a thousand genomes
and brains. `god.undo_depth` and `god.undo_memory_mb` bound it, and the oldest
records are dropped rather than letting history consume the machine. The current
depth and memory are shown live in the toolbar.

Undo history is deliberately **not** saved into snapshots: it holds agents from
a world state the snapshot no longer contains.

### Brushes

Terrain, water, vegetation, soil nutrients, surface rock, ore and temperature.
Pick one and paint in the world view; each stroke is one undoable act. Radius
and intensity are live, the preview ring is drawn at true world scale, and
brushes fall off smoothly to zero at the rim so strokes blend rather than
stamping a hard disc. While a brush is active it owns the left mouse button and
panning falls back to the middle button.

Some brushes do more than write a number. Raising a seabed above sea level
drains it; sinking a plain floods it. Painted ore is placed patchily rather than
as a uniform disc, so a placed vein still looks prospected.

### Disasters

| | |
| --- | --- |
| **Drought** | Strips soil moisture and suppresses rainfall for a period. Biomass falls where it bites and recovers when it lifts, on the ecology's own terms. |
| **Flood** | Raises standing water, saturates soil, drowns vegetation and agents. |
| **Storm** | Heavy rain, flattened vegetation, some mortality. |
| **Wildfire** | Burns the standing crop and **spreads outward** for two days, stopping at water and bare ground. Burnt ground is briefly *more* fertile — the ash returns nutrients. |
| **Earthquake** | Displaces ground along a noise-defined fault trace, not a uniform lift. |
| **Volcano** | Builds a basalt cone, sterilises the area, emplaces sulfur, and triggers a three-year **volcanic winter worldwide**. |
| **Meteor** | Excavates a crater with an uplifted rim and starts an eight-year impact winter. |
| **Ice age** | Drops global temperature for a long period. Ice sheets advance from the poles on their own; nothing about the ice is painted. |
| **Plague** | An imposed mortality event. Resistance comes from the ImmuneStrength trait, so a healthy immune profile genuinely helps. **Not transmissible and cannot coevolve** — real pathogens as evolving entities are still to come, and the UI says so. |

Several disasters persist rather than resolving instantly, and their global
forcing (temperature offset, rainfall multiplier) is applied on top of the
physical climate model rather than replacing it — latitude, season and lapse
rate all still do their work underneath. Ongoing disasters and the current
forcing are listed live.

### Population tools

A shared filter selects by region, life stage, sex expression, age range, any
trait range, tagged-only, and whether immortals are spared. It reports in plain
language what it selects, and the selection is resolved once up front in
ascending slot order so it cannot shift as the operation mutates the store.

Mass kill, mass edit (add/set/scale any trait), forced bottleneck, forced
migration, teleport, mass sterilise and mass fertilise all run through it.

**Selection pressure** is the one that is not engineering. Individuals far from
a favoured trait value die with a probability set by the strength; the survivors
are a biased sample of what was already there, so the trait shifts across
generations rather than instantly, and only if the variation exists to select
on. A **forced bottleneck** is deliberately indifferent to fitness — that
indifference is the point, because a bottleneck is drift, and it shows up
afterwards as a collapse in heterozygosity.

Mass edit writes the expressed **phenotype**, not the genome, so it is not
inherited. To change what descendants get, edit alleles in the Genome Browser
or apply selection pressure.

### Rule overrides

Metabolism, mutation rate, lifespan, gestation and learning-rate multipliers;
gravity; and switches to disable aging, violence and death. Each is a live
config value applied on the next tick and recorded in the log. Disabling death
holds an agent at the brink rather than making it invulnerable, so you can watch
what would have killed it. Disabling violence suppresses the attack action while
leaving aggression free to evolve in the genome — it simply cannot be expressed.

`rules.reaction_rate_multiplier` is registered but has nothing to act on until
the chemistry engine exists; the tooltip says so rather than implying an effect.

### Miracles

A miracle is a recorded sequence of divine acts, replayable on demand and
bindable to `Ctrl+0`..`Ctrl+9`. Turn on recording, perform the acts, name it and
save. Each step lands on the undo stack in its own right, so a miracle can be
unwound one act at a time. Miracles are saved with the world.

### Lua console

Lua 5.4.7, vendored and compiled from source (32 translation units; the
interpreter and compiler mains, the amalgamation and the internal test harness
are excluded). Total additional dependency: none — it links into the same
single executable.

Scripts run **on the sim thread at a tick boundary with the world lock held**,
exactly like any other divine act, so a script sees a coherent world. The cost
is that a script blocks the simulation while it runs, which is why execution is
bounded by an instruction-count hook (`lua.instruction_budget`); a runaway loop
is stopped rather than hanging the sim.

`io` and `os` are deliberately absent, along with `load`, `loadfile`, `dofile`
and `require`: a simulation console that can delete files is not a simulation
console.

The API:

```
world.size()                    world.tick()      world.date()
world.tile(x, y)                world.setTile(x, y, field, value)
pop.count()                     pop.uids()
agent.get(uid)                  agent.set(uid, field, value)
agent.trait(uid, trait)         agent.setTrait(uid, trait, value)
agent.kill(uid)                 agent.spawn(x, y, heterogametic)
agent.attraction(a, b)          agent.relatedness(uidA, uidB)
god.act{kind=..., x=, y=, radius=, intensity=, f0=, i0=}
god.undo()
config.get(key)                 config.set(key, value)     config.keys()
traits                          godActions                 help([filter])
```

The console has history on the up/down arrows, Tab completion over every API
name and trait name, a multi-line scratchpad, and an API tab listing every
binding with one-line documentation.

Headless runs are scriptable too: `--script PATH` runs a chunk once after the
world is built. That is how god mode is exercised without a GUI, and how the
verification below was done.

### Verified

- Terrain brush changed tile elevation from −207.3 m to −30.2 m.
- Ore brush placed hematite; `god.undo()` removed it and the tile read `none`.
- Spawn took the population from 20 to 32.
- Agent trait read 1.069, written to 2.5, read back 2.5.
- Attraction returned score, acceptance and threshold for an ordered pair.
- Ice age reported "12 C colder for 50 years" and imposed the global offset.
- **Mass kill of all 20 founders, then undo**: population 20 → 0 → 20, with the
  sampled individual restored to identical name, position, energy, Size,
  Ornament1 and mother uid.
- Determinism holds with god mode compiled in: same seed → byte-identical
  snapshot, both with and without a startup script.
- Save/resume carries god state (miracles and active disasters).

**Not visually verified:** the god toolbar, brush painting and the Lua console
panel compile and the application runs stably with all of them active, but their
rendered appearance and interaction were not confirmed on screen.

---

## Chemistry, discovery and culture (M6)

### The chemistry is real, and it is data

`data/elements.csv` holds 48 real elements with their measured atomic masses,
electronegativities, densities, melting and boiling points, thermal and
electrical conductivities and Mohs hardness. `data/chemistry.json` holds 82
substances with real formation enthalpies, standard entropies and heat
capacities, and 45 reactions with real activation energies. Both are plain,
editable files. Everything the simulation concludes about what is possible is
computed from them at run time.

**An unbalanced equation cannot be loaded.** The balance check runs per element
at load time and reports the exact discrepancy:

```
reaction 'Carbothermic reduction of hematite' does not balance -- O: 3 on the left, 2 on the right
```

The program then refuses to start rather than running a chemistry that creates
matter from nothing. There is no lenient path and no skip-the-bad-line option.

Nuclear reactions are the one exemption, and the exemption is principled rather
than convenient. A nuclear reaction transmutes elements, so checking *elemental*
balance would be checking the wrong law. What it conserves is nucleon number and
charge -- and not mass, because the mass defect is precisely the energy released.
Those are what get checked instead, and the energy comes from a stated
`energyMeV` converted at 96.485 kJ/mol per MeV.

### Feasibility, rate, catalysis, equilibrium

- **Feasibility** is `dG = dH - T.dS`. Nothing is gated by a tech tier.
- **Le Chatelier** is `dG(P) = dG* + R.T.dn_gas.ln(P/P*)`, applied wherever the
  number of gas molecules changes. This is why Haber-Bosch is worth pressurising
  rather than merely worth heating: at 700 K and 1 atm its dG is +46.8 kJ/mol, and
  at 150 atm it is -11.4.
- **Rate** is Arrhenius, `k = A.exp(-Ea/RT)`. A catalyst substitutes a lower Ea
  without being consumed. Catalysed Haber-Bosch runs at 1.21e+03; uncatalysed, at
  1.02e-07. Same reaction, same thermodynamics, ten orders of magnitude apart.
- **Electrolysis** is exempt from the spontaneity gate when current is supplied,
  because that is what supplying current means. Without it, Hall-Heroult reports
  "needs an electric current" and stops.

Every one of these is inspectable in the **Chemistry Lab** (F7), which has a
runner that evaluates any reaction under conditions you choose and shows dH, dS,
-T.dS, dG before and after the pressure shift, K, the Ea actually used, and the
rate. It calls exactly the `Chemistry::evaluate()` the agents call, so what it
shows is what they experience. A "find crossover temperature" button bisects on
dG to report the furnace you would actually have to build.

### Materials come from composition and history

`Materials::evaluate()` takes a composition and an ordered list of process steps
and computes density, hardness, tensile strength, toughness, melting point,
conductivities and corrosion resistance. Nothing is looked up by name.

Bronze is harder than copper because tin atoms are a different size from copper
atoms, strain the lattice, and impede dislocation motion -- Fleischer
solid-solution strengthening, hardening with the square of the size mismatch and
the square root of concentration. The consequence is that 8% tin bronze comes out
hard and 40% tin bronze comes out brittle rubbish, which is correct, and neither
result is written down anywhere.

Process history is ordered, so quench-then-temper is not the same material as
temper-then-quench. The **Materials** tab of the Chemistry Lab lets you build any
two-component alloy, apply any sequence of steps, and see the result with an
explanation of which term dominated.

### Discovery is a search, not a lookup

There is no recipe list in this program. An agent gathers whatever the tile it is
standing on offers -- ore, bedrock, vegetation, water -- and when its `Use`
output fires and it has accumulated enough curiosity, everything it is holding is
brought together at the hottest temperature its **techniques** can produce. The
engine then returns every reaction whose reactants are present and which would
actually proceed under those conditions, and one is drawn.

Techniques are the ladder, and each one has to be worked out: Mixing, Wetting,
Grinding, Open fire (~900 K), Banked fire (~1200 K, reducing), Kiln (~1400 K),
Bellows (~1700 K), Containment, Electricity. They are themselves taught and
transmitted, which is why fire has to spread before anything depending on fire
can.

An experiment costs energy and discharges the curiosity drive, so exploring is a
real trade against foraging rather than a free action. The reward is paid for
**novelty**, not for usefulness -- which is what makes it worth doing before you
know what it will turn up.

Here is an actual unguided run (seed 4242, 192x192, 240 founders, death
suspended so demography does not obscure the mechanic):

```
  tick     1000  Combustion of wood                           by Founder-81
  tick     1500  Pyrolysis of wood to charcoal                by Founder-81
  tick     1544  Roasting of sphalerite                       by Founder-85
  tick     1750  Incomplete combustion of carbon              by Founder-81
  tick     2025  Combustion of carbon                         by Founder-49
  tick     3001  Decomposition of malachite                   by Founder-184
  tick     3501  Reduction of copper oxide by carbon monoxide by Founder-184
  tick     5294  Firing of clay                               by Founder-119
  tick     5507  Calcination of limestone                     by Founder-146
  tick     5519  Roasting of galena                           by Founder-35
  tick     6257  Slaking of quicklime                         by Founder-146
  tick     7269  Carbothermic reduction of copper oxide       by Founder-35
  tick     7855  Reduction of litharge                        by Founder-107
  tick    29750  Carbothermic reduction of zinc oxide         by Founder-164
```

Fire, then charcoal, then roasting sulfide ores, then copper, then lime and fired
clay, then lead, then zinc last and by a long way. Iron does not appear, because
only 89 of 587 agents ever reached a bellows furnace. **None of that ordering is
scheduled anywhere.** It is what the free energies produce when you let agents
combine what they can reach at the temperatures they can make.

### Culture is tracked separately from genetics

A knowledge unit is concrete -- these inputs, this hot, gave this -- and carries a
`fidelity`. Teaching copies it with loss, and below 0.35 fidelity it stops
working: half-remembered rather than absent. Memory is finite, so when an agent
is full the least-valued unit is displaced, which means useless knowledge is lost
first.

Knowledge is cleared when its holder's slot is released. A technique known only
to individuals who die without teaching anyone is genuinely **lost** and has to be
found again from nothing. That is not a reporting convention; it is what the
memory does.

It is easy to demonstrate. Save the run above at year 3, then resume it with
death switched back on:

```
Culture: 13 first discoveries, 2 knowledge units held, technology index 0.333
  tick     1000  Combustion of wood                    holders 0  [LOST]
  tick     1500  Pyrolysis of wood to charcoal         holders 1
  tick     1544  Roasting of sphalerite                holders 1
  tick     1750  Incomplete combustion of carbon       holders 0  [LOST]
  ...
```

Eleven of thirteen discoveries destroyed by a demographic collapse, the last
survivor holding two. The **Knowledge & Culture** panel (F8) shows this live:
technique spread across the population, and every first discovery with its tick,
its discoverer by name, and its current holder count.

### Where to see it in the UI

- **Chemistry Lab** (F7) -- Elements, Substances, Reactions with the runner,
  Materials workbench.
- **Knowledge & Culture** (F8) -- technique spread and the first-discovery log.
- **Individual Card -> Memory & Knowledge** -- this individual's techniques
  (each toggleable) and knowledge units with fidelity and valuation, plus
  Forget and a combo to teach it any reaction directly.
- **Individual Card -> Inventory** -- what it is carrying in moles and grams,
  with Drop and Give.
- **Charts** -- seven `culture.*` telemetry series, including
  `culture.knowledge_units`, which can fall.

### Verified

- 48 elements, 82 substances, 45 reactions, **0 unbalanced**.
- Hematite reduction by CO: blocked at 600 K ("needs 1200 K, only 600 K
  available"), proceeds at 1500 K.
- Hall-Heroult: proceeds at 1300 K with current, "needs an electric current"
  without.
- Haber-Bosch: dG -11.4 and rate 1.21e+03 catalysed at 150 atm; 1.02e-07
  uncatalysed.
- U-235 fission: dG -19538.2 kJ/mol, from the stated mass defect rather than
  from formation enthalpies.
- The 14-discovery sequence above, emergent and reproducible.
- Determinism holds with M6 active: same seed and script -> byte-identical
  snapshot and CSV, verified twice.
- Save at year 3 and resume: the discovery record survives with correct ticks
  and discoverer names, and holder counts recompute against the living.
- Cost: the chemistry stage is ~2% of tick time at 550 agents (332 ticks/s with
  it inert, 325 with it fully active on the same seed).

**Not visually verified:** the Chemistry Lab, the Knowledge & Culture panel and
the two new Individual Card tabs compile and the application runs stably with
them active, but their rendered appearance and interaction were not confirmed on
screen.

---

## Speciation and phylogeny (M7)

Nothing in this program declares a species. There is no species field on an agent
and no roster file. There is a population of genomes that drift, and a detector
that measures how far apart they have got.

The clock is the **neutral loci** -- junk and MHC. Coding loci would be wrong in
both directions: two populations under identical selection converge on similar
trait values however long they have been apart, and two under opposite selection
diverge in a generation without being distinct species.

### Two wrong thresholds

Worth writing down, because both looked reasonable and both failed.

An **absolute distance threshold** fails immediately: in any randomly mating
population two individuals differ at every neutral locus, so it either makes
every individual its own species or lumps everything together.

Distance **standardised by pooled variance** fails in exactly the case it is
needed. Once the population is bimodal, the between-group separation is itself
the dominant term in the pooled variance -- so the divergence inflates the very
yardstick it is measured against. Measured: two groups deliberately separated by
3.0 allele units scored 1.9 standardised units and stayed one species at every
threshold that did not also shatter a single population. A metric that goes blind
precisely when there is something to see is worse than useless.

What works is a **gap criterion**. A species boundary is not a distance, it is a
discontinuity: a member of a population always has a close relative, a member of
an isolated one does not. So the scale is the median nearest-neighbour distance --
a within-population quantity that stays small however far the groups separate --
and clusters grow by single linkage at `species.gap_factor` times it. Single
linkage rather than complete linkage on purpose: it asks "is there an unbroken
chain of near-neighbours between these two", which is what a continuous
population is and what an isolated one is not.

### Isolation has consequences

A detected species that meant nothing would be a label. Neutral distance feeds a
fertility multiplier on every cross -- from the pair's distance, never from their
labels, so it is continuous with no cliff at the boundary. Zero penalty below
`species.hybrid_onset`; above it, growing as the square of the excess, because
hybrid breakdown compounds. Once divergence costs fertility, assortative mating
is selected for and a split reinforces itself.

### Verified

`data/scripts/speciation_test.lua` shifts one geographically separate group's
neutral alleles by +3.0 through the Lua genome API and tells the detector nothing:

```
Species: 2 extant of 2 ever named  |  81 crosses paid a hybrid penalty, 39 were prevented by it
  Bathymorpha borealis   extant  born yr 0.1  pop 180  drift 0.308
  Bathymorpha candidus   extant  born yr 0.1  pop 128  drift 0.323  <- Bathymorpha borealis at d=3.025
```

The detector recovered the split, measured it at 3.025 against the 3.0 that was
applied, attached the daughter to the right parent, gave sister species a shared
genus, and the isolation term fired. An ordinary single population on the same
build stays at one species with no penalty at all -- no spurious splits.

The **Phylogeny** panel (F6) draws the species tree against a time axis, with
branch thickness by population and a cross at each extinction, plus an individual
ancestry-and-descent tree from the pedigree. Lineages keep one colour across the
tree, the agent sprites and the Territory overlay.

---

## The economy, which is off (M8)

There is no economy in this world unless one comes into existence. Not "money
exists but nobody has any" -- the concept is absent, and the difference matters:

- The tick's economy stage is guarded **at the call site** by one inline bool
  test. With no economy, no function in `econ/economy.cpp` is entered. Not a cheap
  early return -- not entered. A headless run reports `Economy: none. The module
  was never entered.`
- Every container in the module is empty and unallocated until activation.
- No struct anywhere else has a price, value or wealth field. For a commodity
  currency the wealth *is* the inventory holding of that substance; there is no
  ledger at all. Only fiat allocates one, and only on decree.
- God mode reaches the module through a **nullable pointer it does not own**, so
  `god/` does not depend on `econ/` existing. Deleting the directory means
  deleting that pointer, three enum values and three arms of one switch.

### Barter is hard, which is the point

Enabling barter gives nobody anything. Agents exchange goods only when each values
what the other holds more than what it holds itself -- both sides, or it is a
donation rather than a trade. Valuation is subjective and comes from that
individual's own situation: what reactions it knows that consume the good,
diminishing marginal utility on what it already has, and how hungry it is.

Most encounters therefore produce nothing, and the panel reports that failure rate
as the **coincidence rate**. It is the entire reason money is worth inventing.

### Money is detected, not declared

Three properties, multiplied rather than summed because money needs all three:
share of turnover, how often a taker passes a good on instead of consuming it, and
how many *different* goods it is accepted against. An actual unguided run:

```
  [yr 3.92] MONEY HAS EMERGED. clay is now being accepted by individuals who have no
            use for it, because they know it can be passed on. Nobody decreed this:
            its moneyness index is 0.66, it is accepted against 12 other goods, and
            it is passed on 94% of the times it is taken.

Economy: 15808 exchanges from 590119 willing encounters (2.7% coincidence rate)
  currency: clay   wealth Gini 0.938
```

`INTRODUCE CURRENCY` also exists, and the log records it honestly as an
imposition rather than a discovery. `Abolish the economy` releases every structure
and returns the world to having none.

Three bugs here are worth recording because each was a real defect rather than a
tuning problem. **Excluding the currency from barter** -- intending a separate
market path -- meant that the moment a good became money it stopped being
tradeable, so declaring a currency removed it from the economy. **A refusal-based
acceptance rate** charged a refusal against every good both parties held on every
failed pairing, so the term collapsed to near zero for everything and suppressed
the whole index. And **`statsFor` returned a reference into a vector** that the
next call could reallocate; holding two at once was a use-after-free that only
crashed once the vector happened to grow.

---

## Where the tick goes, and what M9 changed

The profiler (F9) times every stage, is always on, and reads no simulation state --
so it cannot change a run, which is why its numbers are excluded from snapshots.
Every headless run prints the breakdown. The status bar's "bottleneck" is now the
measured dominant stage rather than whichever stage happened to run last.

**At 12,000 agents, all simulated in full: 90.5 ms/tick and 13 ticks/s became
21.5 ms/tick and 48 ticks/s.** At ~400 agents, 2.54 ms became 1.05 ms. No level of
detail, no cohorts, no statistical resolution of distant agents: every agent still
gets all 48 inputs built and its whole brain evaluated every tick.

```
  sense               4.0574 ms   18.8%        act                 3.8154 ms   17.7%
  think               4.0233 ms   18.7%        metabolism          3.6074 ms   16.7%
  neighbour lists     2.9937 ms   13.9%        reproduction        1.5140 ms    7.0%
  chemistry           1.2742 ms    5.9%        physics             0.1231 ms    0.6%
  TOTAL TICK         21.5464 ms
```

That flatness is the result worth pointing at. Nothing is above 19%, which means
there is no longer one thing to fix.

### The largest finding was not an algorithm

`parallelFor` had a single serial threshold of 2048 items, below which a call ran
on the calling thread. That is the right number for a tile update costing
nanoseconds per element, and badly wrong for building an agent's 48 sensory inputs,
which costs microseconds. The consequence: **every parallel agent stage had been
running single-threaded at every population this program had ever reached.** The
threshold is now per-call.

Forcing it everywhere would have been the opposite mistake. `physics` integrates a
position in a handful of operations; parallelising it at these populations made it
*twenty-five times slower*.

### Two of the fixes are model corrections

Both were found by chasing performance and both leave the simulation more
plausible, not less:

- **Perception is bounded** (`agents.max_perceived_neighbours`). An animal in a
  herd of five hundred does not individually track five hundred others. The model
  already said social *memory* was finite; this says attention is too.
- **Social interaction per hour is bounded**
  (`agents.max_interactions_per_tick`). Agents converge on water and forage, so a
  tile can hold dozens of them, and the unbounded version had one individual
  grooming every neighbour within reach in a single simulated hour.

Both mattered enormously, because both stages had been scaling with *local
crowding* rather than with population. Bounding interactions took `act` from
56.9 ms to 4.3 ms; bounding perception took `sense` from 19.7 ms to 4.1 ms.

### The rest, in order of what it bought

- Config constants hoisted out of per-agent loops. `cfg().getF("key")` hashes a
  string and probes a map; six were inside `act`'s inner loop. `CfgRef` resolves a
  key once and still reads the value live, so a god-mode rule change takes effect
  immediately -- caching the *number* would have silently broken mid-run edits. It
  resolves **lazily**, because a file-scope `CfgRef` is constructed before `main`
  calls `registerAllSettings`: resolving in the constructor found nothing, returned
  0 for every read, and walked a capacity-0 vector off its end.
- `act` split into a parallel pass for everything that touches only the acting
  agent and a serial pass for what genuinely contends.
- `act`'s three spatial queries -- aggression, grooming, sharing, all at the same
  radius -- hoisted into one parallel CSR neighbour pass. Two passes rather than a
  per-agent cap, because a cap would drop neighbours silently at exactly the
  density where the program is meant to be scaling.
- Vision sector directions stepped by rotation instead of a `cos`/`sin` pair each.
  The sectors are evenly spaced, so each is the previous one turned by a fixed
  angle and the angle-addition identity gives it exactly: twelve transcendental
  calls per agent per tick become two.
- Insolation tabulated per world row once per tick instead of per agent.

### On "10,000 agents at 60 FPS"

Stated precisely, because the two halves are separate facts. The **interface**
holds 60 FPS at 12,000 agents: the simulation runs on its own thread and the UI
never waits for it, and *Hide render* exists to give the whole frame budget back to
ticking. The **simulation** runs at 48 ticks/s at 12,000 agents on this machine --
so one tick is 21.5 ms, which is above a 16.7 ms frame budget. If the requirement
is read as "one tick must fit inside one frame at 10,000 agents", that is not met
yet; at 10,000 rather than 12,000 the tick is about 18 ms, which is close.

### Verified

- Determinism holds through the whole pass: same seed and script produce a
  byte-identical snapshot AND a byte-identical CSV, checked after every change.
- Parallelising the stages changed no outcome -- the same seed produced an
  identical final population before and after -- which is the evidence that the
  stages really are independent. The sensory rotation change *does* shift results,
  because it is a different and equally valid floating-point evaluation order.
- `Genesis.exe` links against Windows DLLs only: no libstdc++, no libgcc, no
  winpthread. Verified by reading the PE import table.

---

## Milestone status

| | Milestone | State |
| --- | --- | --- |
| **M1** | World, rendering, time controls, camera, save/load | **Complete** |
| **M2** | Agents: genetics, metabolism, reproduction, death, Individual Card | **Complete** (see status below) |
| **M3** | Neural brains, drives, reward learning, Brain Inspector | **Complete** (see status below) |
| **M4** | Sex expression, orientation, attraction matrix, sexual selection | **Complete** (see status below) |
| **M5** | Full god mode, Lua console, undo | **Complete** |
| **M6** | Chemistry, materials, discovery, knowledge and culture | **Complete** |
| **M7** | Charts, population genetics, phylogeny, telemetry, headless | **Complete** |
| **M8** | Optional: barter, currency detection, `INTRODUCE CURRENCY`, markets | **Complete** (dormant by default) |
| **M9** | Optimisation pass to 10k+ agents, profiling report | **Complete** |

**Not visually verified:** the M2-M4 interface (Individual Card, Brain
Inspector, Genome Browser, Population table, agent sprites) compiles and the
application runs stably with all of it active, but its rendered appearance and
interactions were not confirmed on screen.

**M1 delivers:** the tile world (tectonics, erosion, D8 drainage, orographic
climate, six strata, process-based ore, Whittaker biomes); the deterministic
tick engine with the full speed control set, stepping, run-until, bookmarks,
chunked binary snapshots, CSV export and headless batch mode; and the docked
dark UI with the world viewport, 13 working overlays, tile inspector, event
feed, charts, statistics and the generated settings screen.

**Not visually verified:** the Phylogeny, Profiler and Economy panels, the four
newly implemented overlays, and the Chemistry Lab all compile and the application
runs stably with them active (30 threads, 421 MB, responding), but their rendered
appearance and interaction were not confirmed on screen. That caveat has applied
to every panel since M2 and it still does.

**Two things in the interface are honestly labelled as not implemented**, rather
than greyed out behind a milestone number that has already shipped: the disease
overlay (there is no disease model in this build at all) and the pollution
overlay (gaseous reaction products vent to an atmosphere that is not tracked per
tile). Both say so in their own tooltips.

Panels for things that do not exist yet say so plainly rather than showing
zeroes that look like measurements. The Statistics window's population-genetics
section states which milestone activates it; overlays that need agents are
greyed out with a tooltip naming the milestone; the god toolbar lists what it
does not yet have.

### The economy is switched off, structurally

There is no economy in this simulation unless one comes into existence. Not
"money exists but nobody has any" — the concept is absent. There is no economy
stage in the tick order, no price field on any struct, no currency member on any
record, and no `ECON` chunk in a save file from a world that never had money.
The Economy panel says exactly that, and its `INTRODUCE CURRENCY` button is
disabled with the milestone that will enable it.

It can only ever switch on two ways: agents converge on a medium of indirect
exchange entirely on their own and the detector fires, or you decree one. M8 is
built last and built so that deleting it entirely would leave M1–M7 a finished
product.

The same rule applies to property law, formal government, organised religion,
written language, contracts, taxation and slavery. None are baseline features.

---

## Layout

```
src/
  main.cpp                  entry, CLI parsing, headless vs windowed
  core/
    rng.h                   xoshiro256++, SplitMix64, per-subsystem stream bank
    jobs.h                  persistent worker pool, deterministic parallelFor
    config.h/.cpp           registry of every tunable, INI load/save
    noise.h/.cpp            Perlin, fBm, ridged, billow, domain warp
    spatial_hash.h          uniform grid, counting-sort rebuild        [for M2]
    serialize.h/.cpp        chunked binary reader/writer
  sim/
    time.h/.cpp             calendar, tick clock, speed control, run-until
    world.h/.cpp            tile SoA, environment schedule, statistics
    worldgen.cpp            tectonics, erosion, rivers, climate, strata, ore
    simulation.h/.cpp       sim thread, command queue, event log, snapshots
  god/
    god.h/.cpp              god actions, brushes, disasters, undo/redo, miracles
    lua_api.h/.cpp          embedded Lua 5.4 console and the simulation API
  ui/
    app.h/.cpp              Win32 + WGL + ImGui shell, docking layout, shortcuts
    viewport.h/.cpp         camera, CPU raster, overlays, picking, tile inspector
    panels.cpp              time bar, god toolbar, events, charts, stats, settings
data/   config.ini
vendor/ imgui (docking branch), lua (5.4.7)
ARCHITECTURE.md             threading, memory, tick order, determinism
CMakeLists.txt  build.bat  README.md
```

[ARCHITECTURE.md](ARCHITECTURE.md) is the design contract the code is written
against and is worth reading before changing anything in `core/` or `sim/`.

---

## Tuning-parameter reference

Full list with descriptions and ranges: `data/config.ini`, or *View → Settings*
in the application. The ones worth knowing:

| Key | Default | Effect |
| --- | --- | --- |
| `world.width` / `world.height` | 1024 | Tiles. ~64 bytes each |
| `world.tile_metres` | 50 | Physical scale of everything |
| `world.latitude_span` | 120 | Degrees top to bottom; drives insolation |
| `worldgen.plates` | 12 | Mountain belts and hydrothermal ore follow boundaries |
| `worldgen.continent_fraction` | 0.34 | Land fraction, hit exactly by quantile |
| `worldgen.uplift_strength` | 1800 | Peak orogenic uplift, metres |
| `worldgen.erosion_passes` | 60 | Valley carving at generation |
| `worldgen.ore_richness` | 1.0 | Global multiplier on all deposits |
| `climate.base_temperature` | 14 | Global **area mean**, °C |
| `climate.pole_gradient` | 40 | Equator-to-pole contrast, °C |
| `climate.axial_tilt` | 23.44 | Zero removes seasons entirely |
| `climate.orographic_strength` | 1.4 | Rain-shadow intensity |
| `climate.prevailing_wind_deg` | 270 | Direction wind blows *from* |
| `ecology.plant_growth_rate` | 0.06 | Logistic rate per daily step |
| `env.*_period_ticks` | 6 / 24 / 720 / 8640 | Environment integration cadences |
| `sim.worker_threads` | 0 (auto) | Baked into saves; changing it breaks bit-exactness |
| `sim.batch_target_ms` | 2.0 | Wall-clock slice per tick batch at MAX |
| `render.ui_scale` | 1.0 | Global interface scale |
