// econ/economy.h — barter, emergent money, and prices. Optional, and OFF.
//
// THE INERTNESS CONTRACT, which is the most important thing in this file.
//
// The spec's requirement is that there is no economy in this world unless one
// comes into existence -- not "money exists but nobody has any", but the concept
// genuinely absent. It must be possible to run to extinction over thousands of
// years with the economy code never having executed a single instruction. That
// is enforced structurally, not by discipline:
//
//   * The tick's economy stage is guarded at the CALL SITE by `econ.active()`,
//     which is an inline load of one bool. When the economy is off, no function
//     in economy.cpp is entered. Not a cheap early return -- not entered.
//   * Every container below is empty and unallocated until activate() is called.
//     There is no market structure to allocate because there is no market.
//   * No struct anywhere else in the program has a price, value, or wealth field.
//     Agents hold substances because substances are physical; nothing holds money
//     because money does not exist. `currencyHeld` lives HERE, is allocated only
//     when a currency comes into being, and does not exist before that.
//   * Deleting this directory and the six lines that reference it leaves M1-M7 a
//     complete program. That was the design constraint, and it is why barter
//     lives here rather than with the other agent behaviours.
//
// HOW MONEY IS SUPPOSED TO ARRIVE.
//
// Barter first, and barter is hard: it needs a double coincidence of wants. A
// has flint and wants grain, B has grain and wants flint. Most pairs do not
// match, and the detector below measures exactly that failure rate.
//
// The way out, historically, is that some good starts being accepted by someone
// who does not want it FOR ITSELF -- they take it because they know they can pass
// it on. That is the whole of what money is, and it is a measurable property of a
// good rather than a decree: high acceptance, low consumption, high turnover. So
// the detector watches for a good that is being acquired and passed on rather
// than acquired and used, and when one crosses the line it says so.
//
// It may never happen, and if it does not, this panel says so rather than
// inventing a currency to have something to display.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gen {

class World;
class Agents;
class RngBank;
class BinaryWriter;
class BinaryReader;
class Inventory;
class KnowledgeBase;

// One completed exchange. Kept in a ring buffer, because the history is for
// inspection and price formation, not for accounting.
struct ExchangeRecord {
    uint64_t tick = 0;
    uint64_t uidA = 0, uidB = 0;
    uint16_t goodA = 0, goodB = 0;    // substance ids; goodB == 0 means paid in currency
    float    amountA = 0.0f, amountB = 0.0f;
    float    currencyPaid = 0.0f;     // 0 unless a currency exists and was used
    // The ratio A gave up per unit received. This is the price, and it exists in
    // barter too -- a price does not need money, only an exchange rate.
    float    rate = 0.0f;
};

// What the detector knows about one good.
struct GoodStats {
    uint16_t substance = 0;
    uint64_t acquiredByTrade = 0;   // times someone took it in an exchange
    uint64_t passedOn = 0;          // times someone traded away what they took
    uint64_t timesOffered = 0;
    uint64_t timesRefused = 0;
    // Which other goods this one has ever been traded AGAINST. Breadth of
    // acceptance is the third property of money and the one hardest to fake:
    // a commodity is swapped for the two or three things its users want, and
    // money is swapped for everything.
    std::vector<uint16_t> counterGoods;

    // 0..1. High when a good turns over heavily, is passed on rather than
    // consumed by whoever takes it, and is accepted against a wide range of
    // other goods. That conjunction is the operational definition of money.
    double moneyness = 0.0;
    // Consecutive detection passes above the threshold. A good has to hold the
    // property, not touch it once.
    uint32_t sustainedPasses = 0;

    double meanRate = 0.0;          // mean exchange rate against everything else
    uint64_t trades = 0;
};

class Economy {
public:
    // -- the inertness gate --------------------------------------------------
    // Inline, so the tick's guard compiles to a load and a branch and no call
    // into this translation unit at all.
    bool active() const { return m_active; }
    bool barterEnabled() const { return m_barter; }
    bool hasCurrency() const { return m_currency != 0; }
    uint16_t currency() const { return m_currency; }
    const std::string& currencyName() const { return m_currencyName; }

    // Turns the module on. Until this is called nothing here is allocated and
    // nothing here runs.
    void activateBarter(size_t maxAgents);
    // Decrees a currency. `substance` 0 means fiat -- a pure token backed by
    // nothing, which is what a god declaring money actually creates.
    void introduceCurrency(uint16_t substance, const std::string& name, size_t maxAgents,
                           float initialPerAgent);
    void deactivate();

    // -- the tick stage ------------------------------------------------------
    // Barter between neighbours. Serial, like every other stage that mutates
    // more than one agent.
    void step(World& world, Agents& agents, RngBank& rng, uint64_t tick,
              std::vector<std::string>& eventsOut);

    // The detection pass, on its own cadence. Reports whether a good has begun
    // behaving as money.
    void detectMoney(uint64_t tick, std::vector<std::string>& eventsOut);

    // -- what an individual thinks something is worth -------------------------
    // Subjective and computed from that individual's own situation: what it can
    // do with the good, how much it already has, and how hungry it is. Gains
    // from trade exist precisely because this differs between individuals.
    double valuation(const Agents& agents, uint32_t slot, uint16_t substance) const;

    // -- reporting ------------------------------------------------------------
    const std::vector<ExchangeRecord>& exchanges() const { return m_exchanges; }
    const std::vector<GoodStats>& goods() const { return m_goods; }
    const GoodStats* good(uint16_t substance) const;
    uint64_t totalExchanges() const { return m_totalExchanges; }
    uint64_t totalOffers() const { return m_totalOffers; }
    double   coincidenceRate() const {
        return m_totalOffers ? static_cast<double>(m_totalExchanges) /
                               static_cast<double>(m_totalOffers) : 0.0;
    }
    float currencyHeld(uint32_t slot) const {
        return slot < m_currencyHeld.size() ? m_currencyHeld[slot] : 0.0f;
    }
    // Price of a good in currency units, or 0 if there is no currency or no
    // trades to derive one from. Derived from exchanges, never assigned.
    double price(uint16_t substance) const;
    // Wealth inequality. Needs the agents, because for a commodity currency the
    // wealth is the holding of that substance and lives in inventories -- there
    // is no separate ledger, and inventing one would be exactly the hidden
    // bookkeeping the inertness contract rules out. Returns -1 when there is no
    // currency, so callers can say "not applicable" instead of showing 0.
    double giniCoefficient(const Agents& agents) const;

    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);

private:
    // Returns an INDEX, not a reference. A reference into m_goods is invalidated
    // by the next call that has to append, and holding two of them at once is a
    // use-after-free that only shows up once the vector happens to grow -- which
    // is exactly how it showed up here.
    size_t statsIndex(uint16_t substance);
    void recordExchange(const ExchangeRecord& r);

    bool m_active = false;
    bool m_barter = false;
    uint16_t m_currency = 0;
    std::string m_currencyName;
    uint64_t m_currencyTick = 0;
    bool m_currencyEmergent = false;   // did it arise, or was it decreed?

    std::vector<ExchangeRecord> m_exchanges;   // ring buffer
    size_t m_exchangeHead = 0;
    std::vector<GoodStats> m_goods;
    std::vector<float> m_currencyHeld;         // allocated only with a currency
    // Per (agent, good): did this individual take it in trade and still hold it?
    // That is what distinguishes passing a good on from using it.
    std::vector<uint8_t> m_tookInTrade;
    size_t m_substanceCount = 0;

    uint64_t m_totalExchanges = 0;
    uint64_t m_totalOffers = 0;
};

}  // namespace gen
