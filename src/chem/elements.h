// chem/elements.h — the periodic table subset and the formula parser.
//
// Element properties are real measured values, loaded from data/elements.csv so
// they can be inspected and edited. They are not decoration: melting point
// decides whether a fire can smelt an ore, electronegativity decides which
// reductions are plausible, thermal and electrical conductivity decide what a
// material is good for, and hardness feeds directly into tool effectiveness.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gen {

struct Element {
    uint8_t     z = 0;              // atomic number
    std::string symbol;
    std::string name;
    double atomicMass = 0.0;        // u
    double electronegativity = 0.0; // Pauling; 0 means "none defined" (noble gases)
    std::string oxidationStates;    // e.g. "+2,+3"
    double density = 0.0;           // g/cm3 at STP
    double meltingPoint = 0.0;      // K
    double boilingPoint = 0.0;      // K
    double thermalConductivity = 0.0;   // W/(m.K)
    double electricalConductivity = 0.0; // MS/m; 0 for insulators
    double hardness = 0.0;          // Mohs; 0 where not meaningful (gases, liquids)
    std::string category;           // metal, nonmetal, metalloid, noble gas, ...

    // The most common oxidation state, parsed from `oxidationStates`. Used by
    // the charge-conservation check.
    int commonOxidationState() const;
};

// One term of a chemical formula: an element and how many atoms of it.
struct FormulaTerm {
    uint8_t z = 0;
    double  count = 0.0;   // fractional counts appear in non-stoichiometric phases
};

// A parsed formula: the elemental composition plus the net charge.
struct Formula {
    std::vector<FormulaTerm> terms;
    int    charge = 0;          // net ionic charge, e.g. -2 for SO4^2-
    double molarMass = 0.0;     // g/mol, computed from the terms
    bool   valid = false;
    std::string error;

    double countOf(uint8_t z) const;
    // Total atoms, used by the balance check.
    double totalAtoms() const;
};

class ElementTable {
public:
    static ElementTable& instance();

    // Loads from CSV. Returns false and fills `error` on a malformed row --
    // silently skipping a bad line would leave the chemistry quietly wrong.
    bool load(const std::string& path, std::string& error);
    // Compiled-in fallback, so a missing data file degrades to a working table
    // rather than a dead program.
    void loadBuiltin();

    bool loaded() const { return !m_elements.empty(); }
    size_t count() const { return m_elements.size(); }
    const std::vector<Element>& elements() const { return m_elements; }

    const Element* bySymbol(const std::string& symbol) const;
    const Element* byZ(uint8_t z) const;

    // Parses a chemical formula. Handles nested groups -- Ca3(PO4)2 --
    // hydrates written with '.' or '*' -- CuSO4.5H2O -- and trailing charges
    // written as ^2- or ^+.
    Formula parse(const std::string& formula) const;

    // Pretty-prints a composition back to a formula-ish string, for the UI.
    std::string describe(const Formula& f) const;

private:
    ElementTable() = default;
    std::vector<Element> m_elements;
    int m_byZ[128] = {0};   // z -> index+1, 0 = absent
    void reindex();
};

inline ElementTable& elements() { return ElementTable::instance(); }

}  // namespace gen
