// GENESIS — entry point.
//
// Two modes:
//   Genesis.exe                       windowed, double-click, no arguments needed
//   Genesis.exe --headless --years N  no window at all; writes a snapshot and CSV
//
// The executable is built for the Windows GUI subsystem so that double-clicking
// it does not flash a console. Headless mode reattaches to the parent console
// (or allocates one) so its output still lands where you launched it from.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "chem/elements.h"
#include "chem/reactions.h"
#include "core/config.h"
#include "sim/simulation.h"
#include "ui/app.h"

namespace {

struct Options {
    bool        headless = false;
    double      years = 100.0;
    uint64_t    seed = 20260728u;
    int         width = 0;      // 0 = take from config
    int         height = 0;
    std::string snapshotPath = "headless_result.gen";
    std::string csvPath = "headless_telemetry.csv";
    std::string loadPath;
    std::string configPath = "data/config.ini";
    bool        quiet = false;
    uint64_t    traceAgent = 0;
    std::string scriptPath;
    bool        writeConfig = false;
    bool        checkChemistry = false;
    bool        showHelp = false;
    bool        badArgument = false;
    std::string badArgumentText;
};

// The executable is linked for the GUI subsystem so double-clicking it does not
// flash a console window. Command-line modes still need somewhere to print.
//
// If stdout is already a valid handle -- because the process was launched from
// a shell, or with its output piped or redirected to a file -- leave it alone:
// reopening CONOUT$ in that case would send output to the console instead of
// the pipe the caller asked for. Only when there is no stdout at all do we
// attach to the parent console, or allocate one as a last resort.
void attachConsole() {
    const HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) return;

    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) ::AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
}

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](double& out) -> bool {
            if (i + 1 >= argc) return false;
            out = std::strtod(argv[++i], nullptr);
            return true;
        };
        auto nextStr = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (a == "--headless") {
            o.headless = true;
        } else if (a == "--years") {
            double v = 0.0;
            if (!next(v)) { o.badArgument = true; o.badArgumentText = "--years needs a value"; }
            else o.years = v;
        } else if (a == "--seed") {
            double v = 0.0;
            if (!next(v)) { o.badArgument = true; o.badArgumentText = "--seed needs a value"; }
            else o.seed = static_cast<uint64_t>(v);
        } else if (a == "--width") {
            double v = 0.0;
            if (!next(v)) { o.badArgument = true; o.badArgumentText = "--width needs a value"; }
            else o.width = static_cast<int>(v);
        } else if (a == "--height") {
            double v = 0.0;
            if (!next(v)) { o.badArgument = true; o.badArgumentText = "--height needs a value"; }
            else o.height = static_cast<int>(v);
        } else if (a == "--snapshot") {
            if (!nextStr(o.snapshotPath)) { o.badArgument = true; o.badArgumentText = a; }
        } else if (a == "--csv") {
            if (!nextStr(o.csvPath)) { o.badArgument = true; o.badArgumentText = a; }
        } else if (a == "--load") {
            if (!nextStr(o.loadPath)) { o.badArgument = true; o.badArgumentText = a; }
        } else if (a == "--config") {
            if (!nextStr(o.configPath)) { o.badArgument = true; o.badArgumentText = a; }
        } else if (a == "--check-chemistry") {
            o.checkChemistry = true;
        } else if (a == "--write-config") {
            o.writeConfig = true;
        } else if (a == "--trace-agent") {
            double v = 0.0;
            if (!next(v)) { o.badArgument = true; o.badArgumentText = "--trace-agent needs a period"; }
            else o.traceAgent = static_cast<uint64_t>(v);
        } else if (a == "--script") {
            if (!nextStr(o.scriptPath)) { o.badArgument = true; o.badArgumentText = a; }
        } else if (a == "--quiet") {
            o.quiet = true;
        } else if (a == "--help" || a == "-h" || a == "/?") {
            o.showHelp = true;
        } else {
            o.badArgument = true;
            o.badArgumentText = "unrecognised option: " + a;
        }
    }
    return o;
}

void printHelp() {
    std::printf(
        "GENESIS - high-fidelity artificial life simulator\n"
        "\n"
        "  Genesis.exe                     Run windowed (the normal way).\n"
        "\n"
        "Headless batch mode:\n"
        "  --headless                      Run with no window at all.\n"
        "  --years N                       Simulated years to run (default 100).\n"
        "  --seed N                        World seed (default 20260728).\n"
        "  --width N  --height N           World size in tiles (default from config).\n"
        "  --load PATH                     Resume from a snapshot instead of generating a\n"
        "                                  new world; --years then means N years MORE.\n"
        "  --snapshot PATH                 Where to write the final world snapshot.\n"
        "  --csv PATH                      Where to write the telemetry time series.\n"
        "  --quiet                         Suppress per-year progress output.\n"
        "  --trace-agent N                 Print one agent's inner state, plus a\n"
        "                                  population diagnostic, every N ticks.\n"
        "  --script PATH                   Run a Lua chunk once, after the world is built\n"
        "                                  and before the run starts. This is how god mode\n"
        "                                  is driven without a GUI.\n"
        "\n"
        "General:\n"
        "  --config PATH                   Config file to load (default data/config.ini).\n"
        "  --write-config                  Write a fully commented config file and exit.\n"
        "  --check-chemistry               Load and validate the chemistry data, then exit.\n"
        "  --help                          This text.\n"
        "\n"
        "Determinism: one seed plus one intervention log reproduces a run bit-for-bit on\n"
        "the same binary. See ARCHITECTURE.md section 5.\n");
}

}  // namespace

int main(int argc, char** argv) {
    gen::registerAllSettings();

    Options opt = parseArgs(argc, argv);

    if (opt.showHelp || opt.badArgument) {
        attachConsole();
        if (opt.badArgument) std::printf("Error: %s\n\n", opt.badArgumentText.c_str());
        printHelp();
        std::fflush(stdout);
        return opt.badArgument ? 2 : 0;
    }

    // Config is loaded before anything reads a constant. A missing file is not
    // an error: the registry defaults are the same values the shipped file
    // contains, so a bare Genesis.exe in an empty folder still runs correctly.
    gen::cfg().load(opt.configPath);

    if (opt.writeConfig) {
        attachConsole();
        ::CreateDirectoryA("data", nullptr);
        // Emit PRISTINE registry defaults, not whatever the existing file
        // happened to contain. Round-tripping the old file instead -- which an
        // earlier version did, because the config is loaded above before this
        // branch runs -- silently preserves stale values and makes it look as
        // though changed defaults had no effect.
        gen::cfg().restoreDefaults();
        const bool ok = gen::cfg().save(opt.configPath);
        std::printf(ok ? "Wrote %s (registry defaults)\n" : "Could not write %s\n",
                    opt.configPath.c_str());
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

    // Chemistry is loaded before the world, because a world with a broken
    // reaction table is not a world worth generating. A reaction that does not
    // balance is a hard failure, reported with the offending element.
    {
        std::string chemError;
        if (!gen::elements().load("data/elements.csv", chemError)) {
            if (opt.checkChemistry) {
                attachConsole();
                std::printf("Element table: %s\n", chemError.c_str());
            }
            gen::elements().loadBuiltin();
        }
        if (!gen::chem().load("data/chemistry.json", chemError)) {
            attachConsole();
            std::printf("CHEMISTRY FAILED TO LOAD\n  %s\n", chemError.c_str());
            std::fflush(stdout);
            if (opt.checkChemistry) return 1;
            gen::chem().loadBuiltin();
        }
    }

    if (opt.checkChemistry) {
        attachConsole();
        std::printf("Elements loaded : %zu\n", gen::elements().count());
        std::printf("Substances      : %zu\n", gen::chem().substances().size());
        std::printf("Reactions       : %zu (all balanced, or the load above would have failed)\n",
                    gen::chem().reactions().size());
        std::printf("Unbalanced      : %zu\n\n", gen::chem().unbalancedCount());

        // Spot-check the reactions that define the technological arc, at the
        // temperatures they actually need.
        struct Probe { const char* name; double T; bool ignition; bool electricity; bool catalyst; };
        static const Probe kProbes[] = {
            {"Combustion of carbon", 900, true, false, false},
            {"Boudouard equilibrium", 1200, false, false, false},
            {"Decomposition of malachite", 900, false, false, false},
            {"Carbothermic reduction of copper oxide", 1300, false, false, false},
            {"Carbothermic reduction of cassiterite", 1500, false, false, false},
            {"Reduction of hematite by carbon monoxide", 1500, false, false, false},
            {"Reduction of hematite by carbon monoxide", 600, false, false, false},
            {"Calcination of limestone", 1300, false, false, false},
            {"Hall-Heroult process", 1300, false, true, false},
            {"Hall-Heroult process", 1300, false, false, false},
            {"Haber-Bosch synthesis", 700, false, false, true},
            {"Haber-Bosch synthesis", 700, false, false, false},
            {"Fission of uranium-235", 400, false, false, false},
        };
        std::printf("%-46s %6s %10s %10s %s\n", "reaction", "T(K)", "dG kJ/mol", "rate", "verdict");
        for (const Probe& pr : kProbes) {
            const gen::Reaction* r = nullptr;
            for (const gen::Reaction& cand : gen::chem().reactions())
                if (cand.name == pr.name) { r = &cand; break; }
            if (!r) { std::printf("%-46s  MISSING\n", pr.name); continue; }
            gen::ReactionConditions c;
            c.temperature = pr.T;
            c.ignition = pr.ignition;
            c.electricity = pr.electricity;
            c.catalystPresent = pr.catalyst;
            if (r->minimumPressure > 0.0) c.pressure = r->minimumPressure;
            const gen::ReactionResult res = gen::chem().evaluate(*r, c);
            std::printf("%-46s %6.0f %10.1f %10.2e %s\n", pr.name, pr.T, res.deltaG, res.rate,
                        res.conditionsMet ? "PROCEEDS" : res.blockedBy.c_str());
        }
        std::fflush(stdout);
        return 0;
    }

    gen::WorldParams params;
    params.width  = (opt.width  > 0) ? opt.width  : static_cast<int>(gen::cfg().getInt("world.width", 1024));
    params.height = (opt.height > 0) ? opt.height : static_cast<int>(gen::cfg().getInt("world.height", 1024));
    params.seed = opt.seed;
    params.tileMetres   = gen::cfg().getF("world.tile_metres", 50.0f);
    params.seaLevel     = gen::cfg().getF("world.sea_level", 0.0f);
    params.latitudeSpan = gen::cfg().getF("world.latitude_span", 120.0f);

    if (opt.headless) {
        attachConsole();
        gen::Simulation sim;
        sim.m_traceAgentPeriod = opt.traceAgent;
        if (!opt.scriptPath.empty()) {
            FILE* sf = std::fopen(opt.scriptPath.c_str(), "rb");
            if (!sf) {
                std::printf("Cannot open script %s\n", opt.scriptPath.c_str());
                std::fflush(stdout);
                return 2;
            }
            std::string src;
            char buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), sf)) > 0) src.append(buf, n);
            std::fclose(sf);
            sim.setStartupScript(src);
        }
        const bool ok = sim.runHeadless(params, opt.years, opt.snapshotPath,
                                        opt.csvPath, !opt.quiet, opt.loadPath);
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

    gen::App app;
    return app.run(params);
}
