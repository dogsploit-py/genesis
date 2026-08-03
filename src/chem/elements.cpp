#include "chem/elements.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gen {

namespace {
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Splits a CSV line, honouring double-quoted fields so a description
// containing a comma does not shear the row.
void splitCsv(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else quoted = false;
            } else cur += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
}
}  // namespace

int Element::commonOxidationState() const {
    if (oxidationStates.empty()) return 0;
    // The first listed state is the common one, by convention in the data file.
    const char* p = oxidationStates.c_str();
    int sign = 1;
    if (*p == '+') ++p;
    else if (*p == '-') { sign = -1; ++p; }
    return sign * std::atoi(p);
}

double Formula::countOf(uint8_t z) const {
    for (const FormulaTerm& t : terms)
        if (t.z == z) return t.count;
    return 0.0;
}

double Formula::totalAtoms() const {
    double n = 0.0;
    for (const FormulaTerm& t : terms) n += t.count;
    return n;
}

// ---------------------------------------------------------------------------

ElementTable& ElementTable::instance() {
    static ElementTable t;
    return t;
}

void ElementTable::reindex() {
    for (int i = 0; i < 128; ++i) m_byZ[i] = 0;
    for (size_t i = 0; i < m_elements.size(); ++i) {
        const uint8_t z = m_elements[i].z;
        if (z < 128) m_byZ[z] = static_cast<int>(i) + 1;
    }
}

const Element* ElementTable::bySymbol(const std::string& symbol) const {
    for (const Element& e : m_elements)
        if (e.symbol == symbol) return &e;
    return nullptr;
}

const Element* ElementTable::byZ(uint8_t z) const {
    if (z >= 128 || m_byZ[z] == 0) return nullptr;
    return &m_elements[static_cast<size_t>(m_byZ[z] - 1)];
}

bool ElementTable::load(const std::string& path, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }

    m_elements.clear();
    char line[1024];
    int lineNo = 0;
    std::vector<std::string> cols;
    bool sawHeader = false;

    while (std::fgets(line, sizeof(line), f)) {
        ++lineNo;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        splitCsv(s, cols);
        if (!sawHeader) { sawHeader = true; continue; }   // skip the column names

        if (cols.size() < 13) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s line %d: expected 13 columns, found %zu",
                          path.c_str(), lineNo, cols.size());
            error = buf;
            std::fclose(f);
            return false;
        }
        Element e;
        e.z = static_cast<uint8_t>(std::atoi(cols[0].c_str()));
        e.symbol = cols[1];
        e.name = cols[2];
        e.atomicMass = std::atof(cols[3].c_str());
        e.electronegativity = std::atof(cols[4].c_str());
        e.oxidationStates = cols[5];
        e.density = std::atof(cols[6].c_str());
        e.meltingPoint = std::atof(cols[7].c_str());
        e.boilingPoint = std::atof(cols[8].c_str());
        e.thermalConductivity = std::atof(cols[9].c_str());
        e.electricalConductivity = std::atof(cols[10].c_str());
        e.hardness = std::atof(cols[11].c_str());
        e.category = cols[12];

        if (e.z == 0 || e.symbol.empty() || e.atomicMass <= 0.0) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s line %d: element needs Z, symbol and mass",
                          path.c_str(), lineNo);
            error = buf;
            std::fclose(f);
            return false;
        }
        m_elements.push_back(std::move(e));
    }
    std::fclose(f);
    reindex();
    return !m_elements.empty();
}

void ElementTable::loadBuiltin() {
    // A compact fallback covering the elements the shipped reactions need, so a
    // missing data/elements.csv degrades to a working table rather than a dead
    // program. The CSV is the authoritative and complete version.
    struct Row { uint8_t z; const char* sym; const char* name; double m; double en;
                 const char* ox; double d; double mp; double bp; double tc; double ec;
                 double h; const char* cat; };
    static const Row kRows[] = {
        {1,"H","Hydrogen",1.008,2.20,"+1,-1",0.00008988,13.99,20.27,0.1805,0,0,"nonmetal"},
        {6,"C","Carbon",12.011,2.55,"+4,+2,-4",2.267,3823,4098,140,0.061,0.5,"nonmetal"},
        {7,"N","Nitrogen",14.007,3.04,"-3,+5",0.0012506,63.15,77.36,0.02583,0,0,"nonmetal"},
        {8,"O","Oxygen",15.999,3.44,"-2",0.001429,54.36,90.20,0.02658,0,0,"nonmetal"},
        {11,"Na","Sodium",22.990,0.93,"+1",0.968,370.94,1156,142,21,0.5,"alkali metal"},
        {12,"Mg","Magnesium",24.305,1.31,"+2",1.738,923,1363,156,22.6,2.5,"alkaline earth"},
        {13,"Al","Aluminium",26.982,1.61,"+3",2.70,933.47,2792,237,37.7,2.75,"metal"},
        {14,"Si","Silicon",28.085,1.90,"+4",2.329,1687,3538,149,0.00000252,6.5,"metalloid"},
        {15,"P","Phosphorus",30.974,2.19,"+5,-3",1.823,317.3,553.7,0.236,0,0,"nonmetal"},
        {16,"S","Sulfur",32.06,2.58,"-2,+4,+6",2.07,388.36,717.8,0.205,0,2,"nonmetal"},
        {17,"Cl","Chlorine",35.45,3.16,"-1,+7",0.003214,171.6,239.11,0.0089,0,0,"halogen"},
        {19,"K","Potassium",39.098,0.82,"+1",0.862,336.53,1032,102.5,13.9,0.4,"alkali metal"},
        {20,"Ca","Calcium",40.078,1.00,"+2",1.55,1115,1757,201,29.8,1.75,"alkaline earth"},
        {26,"Fe","Iron",55.845,1.83,"+2,+3",7.874,1811,3134,80.4,10.0,4,"transition metal"},
        {29,"Cu","Copper",63.546,1.90,"+2,+1",8.96,1357.77,2835,401,59.6,3,"transition metal"},
        {30,"Zn","Zinc",65.38,1.65,"+2",7.14,692.68,1180,116,16.6,2.5,"transition metal"},
        {50,"Sn","Tin",118.71,1.96,"+4,+2",7.265,505.08,2875,66.8,9.17,1.5,"metal"},
        {82,"Pb","Lead",207.2,1.87,"+2,+4",11.34,600.61,2022,35.3,4.81,1.5,"metal"},
    };
    m_elements.clear();
    for (const Row& r : kRows) {
        Element e;
        e.z = r.z; e.symbol = r.sym; e.name = r.name;
        e.atomicMass = r.m; e.electronegativity = r.en; e.oxidationStates = r.ox;
        e.density = r.d; e.meltingPoint = r.mp; e.boilingPoint = r.bp;
        e.thermalConductivity = r.tc; e.electricalConductivity = r.ec;
        e.hardness = r.h; e.category = r.cat;
        m_elements.push_back(std::move(e));
    }
    reindex();
}

// ---------------------------------------------------------------------------
// Formula parsing
// ---------------------------------------------------------------------------

namespace {

struct FormulaParser {
    const ElementTable& table;
    const char* p;
    const char* end;
    std::string error;

    FormulaParser(const ElementTable& t, const std::string& s)
        : table(t), p(s.c_str()), end(p + s.size()) {}

    double readNumber() {
        if (p >= end || !(std::isdigit(static_cast<unsigned char>(*p)) || *p == '.')) return 1.0;
        char* stop = nullptr;
        const double v = std::strtod(p, &stop);
        p = stop;
        return v;
    }

    // Accumulates one group (up to a closing paren or the end) into `out`.
    bool parseGroup(std::vector<FormulaTerm>& out, double multiplier, int depth) {
        if (depth > 8) { error = "formula nested too deeply"; return false; }

        while (p < end) {
            if (*p == ')' || *p == ']' || *p == '}') return true;

            // A hydrate separator: everything after it is a separate unit with
            // its own leading coefficient, as in CuSO4.5H2O.
            if (*p == '.' || *p == '*') {
                ++p;
                const double n = readNumber();
                if (!parseGroup(out, multiplier * n, depth + 1)) return false;
                continue;
            }

            if (*p == '(' || *p == '[' || *p == '{') {
                const char open = *p++;
                const char close = (open == '(') ? ')' : (open == '[') ? ']' : '}';
                std::vector<FormulaTerm> inner;
                if (!parseGroup(inner, 1.0, depth + 1)) return false;
                if (p >= end || *p != close) { error = "unbalanced brackets"; return false; }
                ++p;
                const double n = readNumber();
                for (const FormulaTerm& t : inner) {
                    FormulaTerm scaled = t;
                    scaled.count *= n * multiplier;
                    out.push_back(scaled);
                }
                continue;
            }

            if (!std::isalpha(static_cast<unsigned char>(*p))) {
                error = std::string("unexpected character '") + *p + "' in formula";
                return false;
            }

            // An element symbol is an uppercase letter followed by up to two
            // lowercase ones. Longest match wins so "Cl" is not read as C + l.
            std::string symbol(1, *p++);
            while (p < end && std::islower(static_cast<unsigned char>(*p)) && symbol.size() < 3) {
                symbol += *p;
                if (table.bySymbol(symbol)) { ++p; }
                else { symbol.pop_back(); break; }
            }
            const Element* e = table.bySymbol(symbol);
            if (!e) { error = "unknown element '" + symbol + "'"; return false; }

            const double n = readNumber();
            FormulaTerm t;
            t.z = e->z;
            t.count = n * multiplier;
            out.push_back(t);
        }
        return true;
    }
};

}  // namespace

Formula ElementTable::parse(const std::string& text) const {
    Formula f;
    if (text.empty()) { f.error = "empty formula"; return f; }

    // Split off a trailing charge: "SO4^2-", "Na^+", "Fe^3+".
    std::string body = text;
    const size_t caret = body.find('^');
    if (caret != std::string::npos) {
        const std::string chargeText = body.substr(caret + 1);
        body = body.substr(0, caret);
        int magnitude = 1;
        int sign = 0;
        for (char c : chargeText) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                magnitude = std::atoi(chargeText.c_str());
            } else if (c == '+') sign = 1;
            else if (c == '-') sign = -1;
        }
        if (sign == 0) { f.error = "charge needs a sign, e.g. ^2-"; return f; }
        f.charge = sign * magnitude;
    }

    FormulaParser parser(*this, body);
    std::vector<FormulaTerm> raw;
    if (!parser.parseGroup(raw, 1.0, 0)) { f.error = parser.error; return f; }
    if (parser.p != parser.end) { f.error = "unbalanced brackets"; return f; }

    // Merge repeated elements, e.g. CH3COOH lists C and H more than once.
    for (const FormulaTerm& t : raw) {
        bool merged = false;
        for (FormulaTerm& acc : f.terms) {
            if (acc.z != t.z) continue;
            acc.count += t.count;
            merged = true;
            break;
        }
        if (!merged) f.terms.push_back(t);
    }
    std::sort(f.terms.begin(), f.terms.end(),
              [](const FormulaTerm& a, const FormulaTerm& b) { return a.z < b.z; });

    f.molarMass = 0.0;
    for (const FormulaTerm& t : f.terms) {
        const Element* e = byZ(t.z);
        if (!e) { f.error = "unknown element in formula"; return f; }
        f.molarMass += e->atomicMass * t.count;
    }
    f.valid = !f.terms.empty();
    if (!f.valid) f.error = "formula contains no elements";
    return f;
}

std::string ElementTable::describe(const Formula& f) const {
    std::string s;
    char buf[64];
    for (const FormulaTerm& t : f.terms) {
        const Element* e = byZ(t.z);
        s += e ? e->symbol : "?";
        if (std::fabs(t.count - 1.0) > 1e-9) {
            if (std::fabs(t.count - std::floor(t.count)) < 1e-9)
                std::snprintf(buf, sizeof(buf), "%.0f", t.count);
            else
                std::snprintf(buf, sizeof(buf), "%.3g", t.count);
            s += buf;
        }
    }
    if (f.charge != 0) {
        std::snprintf(buf, sizeof(buf), "^%d%c", std::abs(f.charge), f.charge > 0 ? '+' : '-');
        s += buf;
    }
    return s;
}

}  // namespace gen
