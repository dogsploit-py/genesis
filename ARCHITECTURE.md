# GENESIS — Architecture

This document is the contract the rest of the codebase is written against. It is
written up front, before the simulation subsystems, because threading model,
memory layout and determinism strategy are decisions you cannot retrofit.

---

## 0. Technology choices (and why)

**GUI: Dear ImGui (docking branch) on a raw Win32 window with a WGL OpenGL
context, using the `imgui_impl_win32` + `imgui_impl_opengl2` backends.**

One-sentence justification, as requested: the GENESIS UI is roughly 90% dense
panel/inspector/table/plot work — genome browsers, weight matrices, attraction
matrices, sortable agent tables — which is exactly what ImGui does natively and
what raygui does not, so ImGui wins on the dominant workload.

The backend pair deserves its own note, because it is the reason there are no
binary dependencies at all:

* `imgui_impl_win32` needs only `user32`/`dwmapi`, which ship with Windows.
* `imgui_impl_opengl2` targets fixed-function OpenGL 1.x, all of which is
  exported directly from the system `opengl32.dll`. There is **no GL loader, no
  GLAD/GLEW, no SDL2, no prebuilt `.lib` to ship**, and no `wglCreateContextAttribsARB`
  dance to obtain a core profile.
* The world viewport is **CPU-rasterised** into a single RGBA texture that is
  uploaded per frame and drawn via `ImGui::Image()`. Overlays, agent sprites and
  selection are composited into that same raster or drawn on top with ImGui's
  draw list. Because the viewport is a CPU raster, it needs no shaders, which is
  what makes the GL2 backend sufficient.

Consequence: `Genesis.exe` links against `opengl32`, `gdi32`, `user32`, `shell32`,
`dwmapi`, `imm32` — all OS DLLs — plus statically linked `libgcc`/`libstdc++`/
`winpthread`. It is a genuine single-file, double-click, no-admin executable.

If the CPU raster ever becomes the bottleneck (it will not before ~4 Mpx of
viewport), swapping `imgui_impl_opengl2` for `imgui_impl_opengl3` is a one-file
change plus a context-attribs call; nothing else in the codebase touches GL.

**Threading: hand-rolled job system, not OpenMP.** The available toolchain
(TDM-GCC 10.3.0) ships without `libgomp`, so `-fopenmp` is not an option here.
This turns out to be the better choice anyway: OpenMP's default scheduling makes
no promises about how iterations are partitioned, which is precisely the thing
that has to be nailed down for bit-reproducibility (see §5).

---

## 1. Process and thread model

```
                 ┌────────────────────────────────────────────────┐
   OS input ───▶ │  MAIN THREAD  (UI + render, ~60 Hz)            │
                 │   Win32 message pump                           │
                 │   ImGui frame build                            │
                 │   viewport CPU raster + texture upload         │
                 │   SwapBuffers                                  │
                 └───────┬───────────────────────▲────────────────┘
                         │ CommandQueue          │ SnapshotBuffer
                         │ (UI ▶ sim, mutex)     │ (sim ▶ UI, mutex+swap)
                 ┌───────▼───────────────────────┴────────────────┐
                 │  SIM THREAD  (owns World, runs tick batches)   │
                 │   drain commands ▶ tick ▶ tick ▶ … ▶ publish   │
                 └───────┬────────────────────────────────────────┘
                         │ JobSystem::parallel_for
                 ┌───────▼────────────────────────────────────────┐
                 │  WORKER POOL  (N = hw_concurrency - 1)         │
                 │   persistent threads, no per-tick allocation   │
                 └────────────────────────────────────────────────┘
```

**Ownership rule: the sim thread owns `World` exclusively.** The UI thread never
dereferences a `World` pointer outside of an explicit read lock, and never
mutates one at all. All UI-originated mutation goes through the command queue.
This is what keeps god-mode interventions deterministic and replayable: an
intervention is not "a write that happened at some point", it is *a command
applied at the top of a specific numbered tick*.

Three channels connect the threads, and only three:

| Channel | Direction | Mechanism | Contention |
| --- | --- | --- | --- |
| `CommandQueue` | UI ▶ sim | `std::mutex` + vector swap | drained once per tick batch |
| `SnapshotBuffer` | sim ▶ UI | double-buffered POD stats + event ring | swap under mutex, ~µs |
| `World` read lock | sim ▶ UI | `std::shared_mutex`, UI takes shared | only while rasterising visible tiles |

The world read lock is the only place the UI can stall the sim. It is taken for
the duration of one viewport raster of the *visible region only* (bounded by
screen pixels, not by world size), and released before the texture upload. At
1024×1024 world and a 1600×900 viewport that is well under a millisecond.

**Frame decoupling.** The render loop never waits for a tick to finish and the
sim loop never waits for a frame. If the sim is running at MAX and producing
40,000 ticks/sec, the UI still redraws at 60 FPS and simply shows the most
recently published snapshot. If the renderer is stalled by the compositor, the
sim keeps ticking. Neither ever blocks on the other beyond the short locks above.

**Tick batching.** The sim thread does not lock/unlock per tick — that would be
pure overhead at MAX speed. It runs a batch sized to a target wall-clock slice
(default 2 ms, config `sim.batch_target_ms`), then publishes. Batch size is
recomputed adaptively from measured tick cost. At 1× the batch is 1 tick and the
thread sleeps between ticks; at MAX the batch is however many ticks fit in 2 ms.

---

## 2. Memory layout

Everything hot is **Structure-of-Arrays** in contiguous `std::vector`s that are
sized once at world creation and never reallocated during a run. There are no
per-tick allocations anywhere in the sim; scratch buffers are owned by the
subsystem that needs them and reused.

### 2.1 World tiles

`World` holds parallel arrays indexed by `tileIndex = y * width + x`. At the
default 1024×1024 that is 1,048,576 entries per array.

| Array | Type | Bytes/tile | Meaning |
| --- | --- | --- | --- |
| `elevation` | `float` | 4 | metres relative to sea level, may be negative |
| `waterDepth` | `float` | 4 | metres of standing water |
| `temperature` | `float` | 4 | °C, current (diurnal + seasonal) |
| `rainfall` | `float` | 4 | mm/yr climatological |
| `soilMoisture` | `float` | 4 | 0..1 saturation |
| `waterTable` | `float` | 4 | metres below surface |
| `flowAccum` | `float` | 4 | upstream contributing cells |
| `biomass` | `float` | 4 | kg plant biomass on tile |
| `sediment` | `float` | 4 | erosion scratch / loose material |
| `flowDir` | `int32` | 4 | index of downstream neighbour, −1 = sink |
| `soilN/P/K` | `uint8`×3 | 3 | nutrient stocks, 0..255 |
| `biome` | `uint8` | 1 | derived classification |
| `oreType` | `uint8` | 1 | see `OreType` enum |
| `oreGrade` | `uint8` | 1 | 0..255 → mass fraction |
| `strataRock[6]` | `uint8`×6 | 6 | rock type per layer, surface-first |
| `strataThick[6]` | `uint16`×6 | 12 | decimetres per layer |

≈ 64 bytes/tile → **~67 MB at 1024², ~1.07 GB at 4096².** The 4096² ceiling is
supported but is a 16-GB-RAM proposition; the default and the tested
configuration is 1024². This is stated again in the README because it is the
single largest memory decision in the project.

Why `uint16` decimetres for strata thickness rather than float: it halves the
strata footprint (which is 28% of the tile budget) and 0.1 m is far finer than
any process in the model resolves.

### 2.2 Agents (M2 onward — reserved now, populated later)

Agents will use the same discipline: a `AgentStore` of parallel arrays with a
free-list of slot indices, stable `AgentId = {slot, generation}` handles so that
a dangling reference is detectable rather than a use-after-free. Genomes live in
one flat `std::vector<Allele>` arena with per-agent `{offset, length}`, so an
entire population's genetics is one cache-friendly block and cloning a genome is
a `memcpy`. No `shared_ptr`, no virtual dispatch per agent, no per-agent heap
node — behaviour dispatch is a switch on a `uint8` action enum, brains are
evaluated by a data-driven interpreter over a flat topology array.

### 2.3 Spatial index

Uniform-grid spatial hash (`core/spatial_hash.h`), cell size ≈ the largest
neighbour query radius. Rebuilt each tick by counting sort: a count pass, a
prefix sum, a scatter pass — all `O(n)`, all allocation-free after the first
tick, and all deterministic because agents are inserted in slot order. This is
the structure that makes 10k agents' neighbour queries affordable.

---

## 3. Tick order

One tick = **one simulated hour**. The order below is fixed and is the canonical
definition of a tick; determinism depends on it never varying.

```
TICK n:
  0.  drain CommandQueue        — god interventions apply here, logged with tick n
  1.  advance clock             — hour/day/month/year/season rollovers, insolation
  2.  environment schedule      — see §4; most entries do nothing on most ticks
  3.  sense                     — build spatial hash, gather each agent's inputs     [M2+]
  4.  think                     — evaluate every brain, every tick, no exceptions    [M3+]
  4b. neighbour lists           — CSR close-range index, built in parallel            [M9]
  5.  act                       — apply motor outputs, resolve conflicts in slot order[M2+]
  6.  physics                   — movement integration, collisions, heat transfer    [M2+]
  7.  metabolism                — energy, hunger, thirst, damage, aging              [M2+]
  8.  reproduction              — courtship resolution, gestation, birth             [M2+]
  9.  chemistry                 — gathering, experiment, teaching                      [M6+]
 10.  economy                   — barter and price formation, IF one exists           [M8]
 11.  death & cleanup           — corpses, nutrient return, slot recycling           [M2+]
 11b. species                   — lineage detection, on its own cadence               [M7]
 12.  telemetry                 — accumulate stats; publish snapshot on batch end
```

Step 4b, the CSR close-range neighbour index, was added in M9: `act` was running
three separate spatial queries per agent over the same radius inside a serial
loop, and none of them depended on anything `act` writes.

**There is no disease stage.** Pathogen transmission was specified and was not
built. `DeathCause::Disease` exists only because the Plague god action uses it,
and the disease overlay says as much in its own tooltip rather than sitting greyed
out behind a milestone number that has already shipped.

Step 9 arrives with M6 and covers chemistry and cultural transmission together,
because in this model they are the same stage: an agent experiments, and what it
learns is then taught. Disease is still outstanding. Everything else runs on
every agent on every tick at every speed.

Two of these stages are **serial by necessity**, and it is worth saying why.
`act` mutates the world — biomass genuinely leaves the tile it was eaten from —
and `reproduction` mutates *other* agents, since a mating changes both parties
and can create a third. `chemistry` is serial for both reasons at once: gathering
takes substances out of the ground, and teaching writes into another agent's
knowledge. Running either in parallel would need locks, which would
cost more than the stages do, or a deferred-command buffer, which would change
conflict-resolution order and therefore break reproducibility. Sensing,
thinking, physics and metabolism — which together dominate the tick — are all
parallel.

**Conflict resolution is by slot index, always.** When two agents contend for the
same food tile, the lower slot index wins. This is arbitrary but it is *fixed*,
which is what determinism requires. It is not a fairness statement; per-agent
ordering bias is broken up by the fact that slots are recycled.

---

## 4. The environment schedule (and why it is not a shortcut)

You asked for full fidelity with no statistical shortcuts, and the agent
simulation honours that literally: every agent's brain is evaluated on every
tick at every speed, and fast-forward is achieved *only* by executing more real
ticks per second. Nothing about an agent at 100× is an approximation of what it
would have done at 1×.

The environment is different, and the difference is physical rather than
budgetary. Glacial advance, sediment deposition and aquifer recharge do not have
meaningful hour-to-hour dynamics; integrating them hourly would not add
fidelity, it would add numerical noise around a signal that only exists at
seasonal-to-millennial scale. So environmental processes are integrated by
**operator splitting at their natural timescale**, with the timestep for each
process chosen to match its physics:

| Process | Cadence | Ticks | Rationale |
| --- | --- | --- | --- |
| Insolation, day/night, air temperature | every tick | 1 | genuinely hourly |
| Tile thermal relaxation (conduction/radiation) | 6 h | 6 | thermal time constant of soil/rock |
| Evaporation, soil moisture, plant growth | 1 day | 24 | photosynthesis integrates daily |
| Rainfall pattern, river flow accumulation | 1 month | 720 | synoptic-to-seasonal |
| Erosion, sedimentation, glaciation, tectonics | 1 year | 8,640 | geomorphic |

Every one of these cadences is a config value (`env.*_period_ticks`) and can be
set to 1 to integrate everything hourly if you want to pay for it. The default
is the physically-motivated schedule.

This is the honest distinction: **agents are never approximated; the environment
is integrated with per-process timesteps and the schedule is exposed and
editable.** The UI's bottleneck readout names which stage dominated the last
batch, so you can see the cost of changing it.

---

## 5. Determinism

The guarantee: **one 64-bit seed plus one intervention log reproduces a run
bit-for-bit**, on the same binary and architecture.

**5.1 Per-subsystem PRNG streams.** `core/rng.h` provides xoshiro256++ with
`SplitMix64` seeding. `RngBank` holds one independent stream per subsystem
(`WorldGen`, `Climate`, `Hydrology`, `Ecology`, `Genetics`, `Mutation`, `Brain`,
`Behavior`, `Repro`, `Disease`, `Chemistry`, `Culture`, `God`, …), each seeded
`SplitMix64(worldSeed ^ streamId * φ64)`. Because streams are independent,
consuming extra randomness in one subsystem — which is exactly what a god-mode
intervention does — cannot shift the number sequence any other subsystem sees.
Spawning 500 agents does not perturb the weather.

**5.2 Deterministic parallelism.** `JobSystem::parallel_for(n, fn)` always
partitions `[0, n)` into exactly `workerCount + 1` contiguous chunks by the same
integer arithmetic, regardless of load or timing. A worker's chunk is a function
of `(n, workerCount, chunkIndex)` and nothing else. Consequences:

* No work stealing, no dynamic scheduling. Load imbalance costs throughput; it
  never costs reproducibility.
* **No floating-point atomics.** Float reductions accumulate into a per-chunk
  slot and are summed in ascending chunk order on the calling thread, because
  float addition is not associative and `+=` from racing threads would give a
  different total every run.
* Parallel loops are strictly read-one-array / write-another. Anything requiring
  cross-element ordering runs serial, or runs parallel into per-chunk buffers
  that are merged in index order.
* `workerCount` is baked into the save file. Loading a snapshot on a machine
  with a different core count restores the *recorded* worker count, because
  changing partitioning changes float summation order and would break bitwise
  reproduction.

**5.3 Floating point.** Compiled without `-ffast-math` / `/fp:fast`; x86-64 SSE2
arithmetic is IEEE-754 correctly rounded, so identical operation sequences give
identical bits. `float` is used for tile fields, `double` only where
accumulating over many terms. No `long double` (its width differs between MSVC
and GCC). Transcendentals (`sin`, `exp`, `pow`) are the one soft spot: libm
implementations differ *between compilers*, so bit-exactness is guaranteed for a
given binary, and the file format records the compiler+version so a mismatch is
reported rather than silently diverging.

**5.4 Time independence.** No simulation logic ever reads a wall clock. Real
elapsed time influences *how many* ticks run, never *what a tick does*.

**5.5 The intervention log.** Every command carries the tick it was applied on.
The log is `(tick, commandType, payload)` records, appended in application
order, saved with the world and replayable against a fresh world of the same
seed. This is simultaneously the undo/redo stack, the "how much did I cheat"
audit trail, and the replay format.

---

## 6. Level of detail

There is no LOD on agents. This is a deliberate rejection of the usual trick and
it follows directly from the fidelity requirement: an agent that is off-screen,
or in a distant region, or in a large group, is simulated exactly like one under
the cursor. There are no cohorts, no aggregate populations, no "distant tribes
resolved statistically". The cost of that decision is that the speed ceiling is
CPU-bound, and the UI reports the honest ceiling rather than faking a multiplier.

LOD exists only in **rendering**, where it changes nothing about the simulation:

| Zoom | Viewport strategy |
| --- | --- |
| ≥ 8 px/tile | per-tile raster, individual agent sprites with ornament/health detail |
| 2–8 px/tile | per-tile raster, agents as single-pixel-ish dots |
| < 2 px/tile | tiles box-filtered down, agents rendered as a density accumulation |

Plus two throughput levers that are honest about what they cost:
`Hide render` (stop rasterising, spend the frame budget on ticks; charts and the
event feed keep updating from snapshots) and `--headless` batch mode (no window,
no ImGui, no GL; writes snapshots and CSV telemetry).

---

## 7. Configuration

No tunable constant is hardcoded in logic. `core/config.h` is a registry: each
constant is declared once with key, description, type, default, and range, and
is read through that registry. Two things fall out for free:

1. `data/config.ini` is loaded at startup and written back on demand, with the
   description as a comment.
2. The Settings screen is *generated* from the registry — every registered
   constant automatically appears with an appropriate widget, its tooltip, its
   range, and a working "restore defaults". Adding a constant in a later
   milestone makes it appear in the UI with no UI code written.

---

## 8. Serialization

Binary snapshot: magic `GENESIS\x1a`, format version, compiler tag, world seed,
worker count, then tag-length-value chunks (`WRLD`, `TILE`, `AGNT`, `GENE`,
`BRAN`, `PEDG`, `KNOW`, `CHEM`, `ECON`, `ILOG`). Unknown chunks are skipped by
length, so an older save loads into a newer build with the missing subsystems
left at defaults, and a chunk added in M6 does not invalidate an M2 save. Tile
arrays are written as raw contiguous blocks — one `fwrite` per array.

`ECON` is written only if a currency exists (§ economy, M8). A moneyless world's
save file contains no economic chunk at all.

---

## 9. Module map

```
src/
  main.cpp                  entry, CLI parsing, headless vs windowed
  core/
    rng.h                   xoshiro256++, SplitMix64, per-subsystem stream bank
    jobs.h/.cpp             persistent worker pool, deterministic parallel_for
    config.h/.cpp           registry of every tunable, INI load/save
    noise.h/.cpp            Perlin, fBm, domain warp, ridged
    spatial_hash.h/.cpp     uniform grid, counting-sort rebuild
    serialize.h/.cpp        chunked binary reader/writer
  sim/
    time.h/.cpp             calendar, tick clock, speed control, run-until, bookmarks
    world.h/.cpp            tile SoA, environment schedule, queries
    worldgen.cpp            tectonics, erosion, rivers, climate, strata, ore
    simulation.h/.cpp       sim thread, command queue, event log, snapshots
    genetics/ brain/ agent/ repro/ social/ culture/ disease/     [M2–M5]
  chem/
    elements.h/.cpp         periodic table subset, formula parser (groups, hydrates, charge)
    reactions.h/.cpp        substances, balance check, dG, Arrhenius, Le Chatelier
    materials.h/.cpp        properties from composition and process history
  sim/
    knowledge.h/.cpp        techniques, knowledge units, discovery record
    chemistry_agent.h/.cpp  inventory, gathering, the experiment, teaching
  econ/  dormant by default                                      [M8]
  god/   commands, undo/redo, miracles, Lua binding              [M5]
  ui/
    chem_ui.cpp             Chemistry Lab, materials workbench, Knowledge & Culture
    app.h/.cpp              Win32 + WGL + ImGui shell, docking layout
    viewport.h/.cpp         camera, CPU raster, overlays, picking
    panels.cpp              time bar, event feed, charts, stats, settings
    cards/ console/                                              [M2+, M5]
data/   config.ini, chemistry.json, elements.csv, species.json
vendor/ imgui (docking)
```

---

## 9a. God mode and the intervention channel (M5)

Every divine power is a `GodAction` value routed through the same command queue
as everything else, and applied on the sim thread at a tick boundary with the
world lock held exclusively. Nothing in the UI mutates simulation state
directly. That is what makes an intervention loggable, replayable, storable in
a miracle, and undoable.

Undo is **delta-based**: each record holds the tiles and the whole agents an
action touched, both before and after. Redo restores the recorded after-state
rather than re-running the action, because re-running would consume randomness
from the God stream and produce a different world. Because the God stream is
independent of every other stream (§5.1), an intervention -- and its undo --
cannot shift the number sequence any other subsystem sees.

The Lua console runs chunks on the sim thread under the same exclusive lock, so
a script observes a coherent world rather than one shifting under it
mid-statement. The cost is that a script blocks the simulation while it runs,
which is why execution is bounded by an instruction-count hook rather than
trusted to terminate.

Undo history is not serialised into snapshots: it holds whole agents belonging
to a world state the snapshot no longer contains, and restoring one into a
reloaded world would resurrect an individual against a stale genome arena.

---

## 9b. Chemistry, discovery and culture (M6)

Chemistry is data, not code. `data/elements.csv` and `data/chemistry.json` carry
real measured quantities -- formation enthalpies, standard entropies, activation
energies, melting points -- and everything the simulation concludes about what is
possible is computed from them at run time.

**A reaction that does not balance cannot be loaded.** The check runs per element
at load, and reports the exact discrepancy. There is no "skip the bad line"
path, because a chemistry that quietly creates matter from nothing would poison
every conclusion drawn downstream. Nuclear reactions are exempt from *elemental*
balance and checked against the laws they actually obey instead: nucleon number
and charge. Not mass -- the mass defect is precisely the energy released.

Feasibility is dG = dH - T.dS, with a Le Chatelier pressure term
dG(P) = dG* + R.T.dn_gas.ln(P/P*) applied wherever the gas mole count changes.
Rate is Arrhenius, k = A.exp(-Ea/RT), and a catalyst substitutes a lower Ea
without being consumed. Electrolysis is exempt from the spontaneity gate when
current is supplied, because that is what supplying current means.

### Discovery is a search

An agent does not look up a recipe, because there is no recipe list. It gathers
substances from the tile it is standing on, and when its `Use` output fires and
it has accumulated enough curiosity, everything it holds is brought together at
the hottest temperature its **techniques** can reach. `Chemistry::findApplicable`
then returns every reaction whose reactants are a subset of what is present and
which would actually proceed under those conditions, and one is drawn.

Techniques are the ladder -- Mixing, Wetting, Grinding, Open fire, Banked fire,
Kiln, Bellows, Containment, Electricity -- and each is itself learned and
transmitted. This is why copper reliably precedes iron in an unguided run: copper
oxide reduces around 1200 K and iron oxide needs about 1700 K, so the bloomery
has to be invented first. Nothing schedules that ordering. It falls out of the
thermodynamics.

### Culture is tracked separately from genetics

A `KnowledgeUnit` is concrete -- these inputs, this hot, gave this -- and carries
a `fidelity`. Teaching copies it with loss, and below 0.35 fidelity it stops
working: half-remembered rather than absent. Knowledge is cleared when its
holder's slot is released, so a technique known only to individuals who die
without teaching anyone is genuinely **lost**, and the world has to find it again
from nothing. The Knowledge & Culture panel reports `holders` per discovery for
exactly this reason.

The stage costs about 2% of tick time at ~550 agents; it is not what makes the
agent stages expensive.

---

## 9c. Speciation and the phylogeny (M7)

Nothing declares a species. There is no species field on an agent and no roster
file; there is a population of genomes that drift and a detector that measures
how far apart they have got. The detector runs on its own cadence
(`species.period_ticks`) because it is a measurement of the population rather
than a process acting on it.

The clock is the **neutral loci** — junk and MHC. Coding loci would be the wrong
measure in both directions: two populations under identical selection converge
on similar trait values however long they have been separated, and two under
opposite selection diverge within a generation without being distinct species.

### Two wrong thresholds, and why the third one works

This is worth recording because both failures looked reasonable.

1. **An absolute distance threshold.** Fails immediately. In any randomly mating
   population two individuals differ at every neutral locus, so an absolute
   threshold either makes every individual its own species or lumps everything
   together, with nothing useful in between.

2. **Distance standardised by pooled per-locus variance.** Fails in exactly the
   case it is needed. Once the population is bimodal, the between-group
   separation is itself the dominant term in the pooled variance — so the
   divergence inflates the very yardstick it is measured against and the ratio
   never crosses a fixed line. Measured: two groups deliberately separated by
   3.0 allele units scored 1.9 standardised units and stayed one species at every
   threshold that did not also shatter a single population.

3. **A gap criterion**, which is what is implemented. A species boundary is not a
   distance, it is a discontinuity: a member of a population always has a close
   relative, and a member of a reproductively isolated population does not. So
   the scale is the **median nearest-neighbour distance** — a within-population
   quantity that stays small however far the groups separate — and clusters are
   grown by single linkage at `species.gap_factor` times it.

Single linkage rather than complete linkage on purpose: it asks "is there an
unbroken chain of near-neighbours between these two", which is what a continuous
population is and what an isolated one is not.

Clustering is quadratic, so it runs on a bounded sample (`species.cluster_sample`).
That is a **reporting** sample in the same sense as the population-genetics
window: every agent is still simulated in full, and every agent is still assigned
to a lineage by nearest centroid afterwards. Only the cluster *discovery* is
sampled.

### Isolation has teeth

A detected species that carried no consequence would be a label. So neutral
distance feeds a fertility multiplier on every cross, from the pair's distance and
never from their labels — continuous, with no cliff at the boundary, zero penalty
below `species.hybrid_onset` and growing as the square of the excess above it
because hybrid breakdown compounds. Once divergence costs fertility, assortative
mating is selected for and a split reinforces itself.

---

## 9d. The optional economy (M8)

The inertness contract is enforced structurally, not by discipline:

* The tick's economy stage is guarded **at the call site** by `econ.active()`, an
  inline load of one bool. With no economy, no function in `econ/economy.cpp` is
  entered — not a cheap early return, not entered.
* Every container in the module is empty and unallocated until activation.
* No struct anywhere else has a price, value or wealth field. Agents hold
  substances because substances are physical. For a commodity currency the
  "wealth" is the inventory holding of that substance and there is no ledger at
  all; only fiat allocates one, and only on decree.
* God mode reaches the module through a **nullable pointer it does not own**, so
  `god/` does not depend on `econ/` existing. Deleting the directory means
  deleting that pointer, three enum values and three arms of one switch.

Money is not a separate mechanism beside barter. It is a good that everyone
accepts, and what makes it money is the valuation premium plus the fact that
others take it. Excluding the currency from barter — which the first version did,
intending a separate market path — had the perverse effect of removing money from
the economy the moment it emerged.

Detection watches three properties, multiplied rather than summed because money
needs all three: share of turnover, the rate at which takers pass a good on
rather than consume it, and the **breadth** of goods it is accepted against. The
first version used an acceptance rate derived from refusals, which was wrong: a
failed pairing charges a refusal against every good both parties held, so the term
collapsed to near zero for everything and suppressed the whole index. Detection is
also gated on minimum diversity and volume, because on a thin economy whatever is
moving looks both dominant and universally accepted — and since detection stops
once a currency exists, an early mistake is permanent.

---

## 9e. Where the tick goes, and what M9 changed

`core/profiler.h` times every stage with a steady_clock pair and a rolling mean.
It is always on, reads no simulation state and consumes no randomness, so it
cannot change a run — which is why the timings are excluded from snapshots. The
status bar's "bottleneck" is now the measured dominant stage rather than
whichever stage happened to run last.

The pass was driven entirely by that instrument, and the largest finding was not
an algorithm:

**`parallelFor` had a single serial threshold of 2048 items.** Below it, calls ran
on the calling thread. That number is right for a tile update costing nanoseconds
per element and badly wrong for building an agent's 48 sensory inputs, which costs
microseconds. The consequence was that *every parallel agent stage ran
single-threaded at every population the program had ever reached.* The threshold
is now per-call, and the expensive stages pass `JobSystem::kAgentGrain`.

Forcing it everywhere would have been the opposite mistake: `physics` integrates a
position in a handful of operations, and parallelising it at these populations made
it twenty-five times slower. The grain is per-call precisely so cheap bodies keep
the conservative default.

The other changes, in order of what they bought:

* Config constants hoisted out of per-agent loops. `cfg().getF("key")` hashes a
  string and probes a map; six of them were inside `act`'s inner loop, which at
  10,000 agents is 60,000 hashes a tick for numbers that cannot change mid-stage.
  `CfgRef` resolves a key once and still reads the value live, so a god-mode rule
  change takes effect immediately — caching the number instead of the lookup would
  have silently broken mid-run rule edits. It resolves **lazily**: a file-scope
  `CfgRef` is constructed during static initialisation, before `main` calls
  `registerAllSettings`, and resolving in the constructor found nothing, returned
  0 for every read, and walked a capacity-0 vector off its end.
* Vision sector directions stepped by rotation instead of a `cos`/`sin` pair each.
  The six sectors are evenly spaced, so each is the previous one turned by a fixed
  angle and the angle-addition identity gives it exactly: twelve transcendental
  calls per agent per tick become two.
* Insolation tabulated per world row once per tick instead of recomputed per agent.
* `act`'s three spatial queries — aggression, grooming, food sharing, all at the
  same radius — hoisted into one parallel CSR neighbour pass. This matters more
  than its share of the total suggests: `act` is **serial**, and serial work is
  what bounds scaling. Two passes rather than a per-agent cap, because a cap would
  silently drop neighbours at high density, which is exactly where the program is
  meant to be scaling.

Parallelising the stages changed no outcome: the same seed produced an identical
final population before and after, which is the evidence that the stages really
are independent. The sensory rotation change does shift results, because it is a
different (equally valid) floating-point evaluation order — still fully
deterministic, just not bit-identical to the previous build.

---

## 10. What is deliberately *not* in the architecture

The economy has no place in this document beyond a note that `econ/` exists and
is empty. That is intentional and structural, not an oversight: a moneyless
world must not pay for market code, so there is no economy stage in the tick
order, no price field on any struct, no currency member on the agent record, and
no `ECON` chunk in a save file from a world that never had money. The subsystem
switches on only via emergence detection or divine fiat, and switching it off
returns the world to a state indistinguishable from one where it never existed.
The same rule applies to property law, government, religion, writing, contracts,
taxation and slavery: none are baseline features, each is either detected as
emergent or imposed through god mode.
