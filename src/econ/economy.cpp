#include "econ/economy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "chem/reactions.h"
#include "core/config.h"
#include "core/serialize.h"
#include "sim/agent.h"
#include "sim/world.h"

namespace gen {

namespace {
constexpr size_t kExchangeHistory = 4096;
}

// ---------------------------------------------------------------------------
// Activation. Nothing above is allocated until one of these runs.
// ---------------------------------------------------------------------------

void Economy::activateBarter(size_t maxAgents) {
    m_active = true;
    m_barter = true;
    // Asked for here rather than passed in, so no caller needs to know about
    // the chemistry to switch the economy on. +1 because substance ids are
    // 1-based and id 0 is reserved for "the currency, which is not a substance".
    m_substanceCount = chem().substances().size() + 1;
    m_exchanges.assign(kExchangeHistory, ExchangeRecord{});
    m_exchangeHead = 0;
    m_goods.clear();
    m_tookInTrade.assign(maxAgents * m_substanceCount, 0u);
    m_totalExchanges = 0;
    m_totalOffers = 0;
}

void Economy::introduceCurrency(uint16_t substance, const std::string& name, size_t maxAgents,
                                float initialPerAgent) {
    if (!m_active) activateBarter(maxAgents);
    m_currency = substance;
    m_currencyName = name.empty() ? "unit" : name;
    m_currencyEmergent = false;
    // Only now does a wealth field exist anywhere in the program.
    m_currencyHeld.assign(maxAgents, initialPerAgent);
}

void Economy::deactivate() {
    m_active = false;
    m_barter = false;
    m_currency = 0;
    m_currencyName.clear();
    m_currencyEmergent = false;
    // Release everything, so switching the economy off returns the program to
    // the state where it genuinely has none.
    m_exchanges.clear();
    m_exchanges.shrink_to_fit();
    m_goods.clear();
    m_goods.shrink_to_fit();
    m_currencyHeld.clear();
    m_currencyHeld.shrink_to_fit();
    m_tookInTrade.clear();
    m_tookInTrade.shrink_to_fit();
    m_totalExchanges = 0;
    m_totalOffers = 0;
}

// ---------------------------------------------------------------------------
// Subjective valuation
// ---------------------------------------------------------------------------

double Economy::valuation(const Agents& agents, uint32_t slot, uint16_t substance) const {
    const Substance* sub = chem().substance(substance);
    if (!sub) return 0.0;

    double v = 0.1;   // everything is worth something, if only as material

    // USE VALUE. Does this individual know a reaction that consumes it? That is
    // the difference between a rock and a reagent, and it is the largest term --
    // which is why knowledge and trade end up coupled: what a good is worth to
    // you depends on what you know how to do with it.
    const KnowledgeBase& kb = agents.knowledge();
    for (const KnowledgeUnit& k : kb.known(slot)) {
        if (!k.usable()) continue;
        const Reaction* r = chem().reaction(k.reactionId);
        if (!r) continue;
        for (const ReactionTerm& t : r->reactants) {
            if (t.substance != substance) continue;
            v += 1.0 * static_cast<double>(k.valuation);
            break;
        }
    }

    // DIMINISHING MARGINAL UTILITY. The tenth unit of something is worth less
    // than the first. This is what makes trade mutually beneficial rather than
    // zero-sum: two individuals with lopsided holdings both gain by evening out.
    const float held = agents.inventory().amountOf(slot, substance);
    v /= (1.0 + 0.35 * static_cast<double>(held));

    // Edible things are worth more to a hungry individual. Valuation is a
    // function of the valuer's state, not a property of the object.
    if (sub->formulaText == "C6H10O5" || sub->formulaText == "H2O") {
        const double hunger =
            static_cast<double>(agents.m_drives[slot][Drive::Hunger]);
        v *= 1.0 + hunger;
    }

    // A good that already circulates is worth holding even if you cannot use it,
    // because you know someone will take it. This is the self-reinforcing part
    // of money, and it is the reason the emergence is a feedback loop rather than
    // a threshold crossing.
    if (const GoodStats* g = good(substance))
        v *= 1.0 + 2.0 * g->moneyness;
    if (substance == m_currency) v *= 3.0;

    return v;
}

// ---------------------------------------------------------------------------
// Barter
// ---------------------------------------------------------------------------

size_t Economy::statsIndex(uint16_t substance) {
    for (size_t i = 0; i < m_goods.size(); ++i)
        if (m_goods[i].substance == substance) return i;
    GoodStats g;
    g.substance = substance;
    m_goods.push_back(g);
    return m_goods.size() - 1;
}

const GoodStats* Economy::good(uint16_t substance) const {
    for (const GoodStats& g : m_goods)
        if (g.substance == substance) return &g;
    return nullptr;
}

void Economy::recordExchange(const ExchangeRecord& r) {
    if (m_exchanges.empty()) return;
    m_exchanges[m_exchangeHead] = r;
    m_exchangeHead = (m_exchangeHead + 1) % m_exchanges.size();
    ++m_totalExchanges;
}

void Economy::step(World& world, Agents& agents, RngBank& rng, uint64_t tick,
                   std::vector<std::string>& eventsOut) {
    (void)world;
    (void)rng;
    if (!m_barter) return;

    const float threshold = cfg().getF("econ.trade_threshold", 0.55f);
    const float range = cfg().getF("econ.trade_range", 2.0f);
    const float minGain = cfg().getF("econ.min_gain", 0.15f);
    const float tradeFraction = cfg().getF("econ.trade_fraction", 0.5f);

    Inventory& inv = agents.inventory();
    const int slots = inv.slots();

    for (uint32_t a : agents.liveSlots()) {
        if (agents.m_stage[a] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
        if (agents.m_stage[a] == static_cast<uint8_t>(LifeStage::Juvenile)) continue;
        const float* outA = &agents.m_outputs[a * kBrainOutputCount];
        // Out_Share is reused as the exchange disposition rather than adding an
        // Out_Trade. The output count is part of the brain genome format and is
        // static_asserted at 20: adding an output would invalidate every brain in
        // every existing save. Reusing the prosocial output is defensible on its
        // own terms -- "engage in exchange with this individual" and "give to this
        // individual" are the same underlying willingness to interact -- and it is
        // gated on a separate threshold so the two behaviours can be tuned apart.
        if (outA[Out_Share] <= threshold) continue;

        bool traded = false;
        agents.forEachNeighbour(agents.m_x[a], agents.m_y[a], range, [&](uint32_t b) {
            if (traded || b == a) return;
            if (agents.m_stage[b] == static_cast<uint8_t>(LifeStage::Embryo)) return;
            if (agents.m_stage[b] == static_cast<uint8_t>(LifeStage::Juvenile)) return;
            // Ascending order only, so each pair is considered once per tick and
            // the outcome does not depend on which of the two came first in the
            // live-slot list.
            if (b < a) return;
            const float* outB = &agents.m_outputs[b * kBrainOutputCount];
            if (outB[Out_Share] <= threshold) return;

            // The double coincidence of wants. Every good A holds against every
            // good B holds -- at most 8x8 with the default inventory, so the
            // exhaustive search is cheaper than being clever about it.
            ++m_totalOffers;
            double bestGain = static_cast<double>(minGain);
            uint16_t bestGiveA = 0, bestGiveB = 0;
            float bestAmtA = 0.0f, bestAmtB = 0.0f;

            for (int i = 0; i < slots; ++i) {
                const uint16_t ga = inv.substanceAt(a, i);
                const float qa = inv.amountAt(a, i);
                if (ga == 0 || qa <= 1e-4f) continue;
                for (int j = 0; j < slots; ++j) {
                    const uint16_t gb = inv.substanceAt(b, j);
                    const float qb = inv.amountAt(b, j);
                    if (gb == 0 || qb <= 1e-4f || gb == ga) continue;
                    // A commodity currency is deliberately NOT excluded here.
                    // Excluding it was a real bug and worth recording: it meant
                    // that the moment a good became money it stopped being
                    // tradeable, so declaring a currency removed it from the
                    // economy. Money is not a special mechanism bolted on beside
                    // barter -- it is a good that everyone accepts, and the only
                    // thing that makes it money is the valuation premium in
                    // valuation() plus the fact that everyone else takes it.

                    // Gains from trade: each side must value what it receives
                    // more than what it gives up. Both conditions, or it is not
                    // an exchange, it is a donation.
                    const double aGives = valuation(agents, a, ga);
                    const double aGets  = valuation(agents, a, gb);
                    const double bGives = valuation(agents, b, gb);
                    const double bGets  = valuation(agents, b, ga);
                    if (aGets <= aGives || bGets <= bGives) continue;

                    const double gain = (aGets - aGives) + (bGets - bGives);
                    if (gain <= bestGain) continue;
                    bestGain = gain;
                    bestGiveA = ga;
                    bestGiveB = gb;
                    bestAmtA = qa * tradeFraction;
                    bestAmtB = qb * tradeFraction;
                }
            }

            if (bestGiveA == 0 || bestGiveB == 0) {
                // No double coincidence of wants. Recorded against every good
                // both parties were holding, because each was in effect offered
                // and not taken -- and that failure rate is the whole reason
                // money is worth inventing.
                for (int i = 0; i < slots; ++i) {
                    const uint16_t ga = inv.substanceAt(a, i);
                    if (ga != 0 && inv.amountAt(a, i) > 1e-4f) {
                        const size_t k = statsIndex(ga);
                        ++m_goods[k].timesOffered;
                        ++m_goods[k].timesRefused;
                    }
                    const uint16_t gb = inv.substanceAt(b, i);
                    if (gb != 0 && inv.amountAt(b, i) > 1e-4f) {
                        const size_t k = statsIndex(gb);
                        ++m_goods[k].timesOffered;
                        ++m_goods[k].timesRefused;
                    }
                }
                return;
            }

            // Execute. Both transfers or neither.
            if (!inv.consume(a, bestGiveA, bestAmtA)) return;
            if (!inv.consume(b, bestGiveB, bestAmtB)) {
                inv.add(a, bestGiveA, bestAmtA);   // put it back
                return;
            }
            inv.add(b, bestGiveA, bestAmtA);
            inv.add(a, bestGiveB, bestAmtB);

            ExchangeRecord rec;
            rec.tick = tick;
            rec.uidA = agents.m_uid[a];
            rec.uidB = agents.m_uid[b];
            rec.goodA = bestGiveA;
            rec.goodB = bestGiveB;
            rec.amountA = bestAmtA;
            rec.amountB = bestAmtB;
            rec.rate = bestAmtB > 1e-6f ? bestAmtA / bestAmtB : 0.0f;
            recordExchange(rec);

            // Both indices are resolved BEFORE either is used, because the
            // second lookup can append and move the vector.
            const size_t ka = statsIndex(bestGiveA);
            const size_t kb = statsIndex(bestGiveB);
            GoodStats& sa = m_goods[ka];
            GoodStats& sb = m_goods[kb];
            // Each good records the other as something it is accepted against.
            if (std::find(sa.counterGoods.begin(), sa.counterGoods.end(), bestGiveB) ==
                sa.counterGoods.end())
                sa.counterGoods.push_back(bestGiveB);
            if (std::find(sb.counterGoods.begin(), sb.counterGoods.end(), bestGiveA) ==
                sb.counterGoods.end())
                sb.counterGoods.push_back(bestGiveA);
            ++sa.trades;
            ++sb.trades;
            ++sa.timesOffered;
            ++sb.timesOffered;
            ++sb.acquiredByTrade;   // A acquired B's good
            ++sa.acquiredByTrade;   // B acquired A's good
            if (rec.rate > 0.0f) {
                sa.meanRate = (sa.meanRate * static_cast<double>(sa.trades - 1) +
                               rec.rate) / static_cast<double>(sa.trades);
            }

            // Was the good A just gave away one it had itself taken in trade?
            // That is the passing-on that distinguishes a medium of exchange from
            // a consumable, and it is the core measurement.
            const size_t ia = static_cast<size_t>(a) * m_substanceCount + bestGiveA;
            if (ia < m_tookInTrade.size() && m_tookInTrade[ia]) {
                ++sa.passedOn;
                m_tookInTrade[ia] = 0;
            }
            const size_t ib = static_cast<size_t>(b) * m_substanceCount + bestGiveB;
            if (ib < m_tookInTrade.size() && m_tookInTrade[ib]) {
                ++sb.passedOn;
                m_tookInTrade[ib] = 0;
            }
            // Mark what each side just took.
            const size_t ja = static_cast<size_t>(a) * m_substanceCount + bestGiveB;
            if (ja < m_tookInTrade.size()) m_tookInTrade[ja] = 1;
            const size_t jb = static_cast<size_t>(b) * m_substanceCount + bestGiveA;
            if (jb < m_tookInTrade.size()) m_tookInTrade[jb] = 1;

            agents.m_action[a] = static_cast<uint8_t>(Action::Share);
            agents.m_action[b] = static_cast<uint8_t>(Action::Share);
            traded = true;

            if (m_totalExchanges == 1) {
                const Substance* s1 = chem().substance(bestGiveA);
                const Substance* s2 = chem().substance(bestGiveB);
                char buf[320];
                std::snprintf(buf, sizeof buf,
                              "FIRST TRADE: %s gave %s to %s for %s. Nobody was told to; both "
                              "wanted what the other had.",
                              agents.m_name[a].c_str(), s1 ? s1->name.c_str() : "?",
                              agents.m_name[b].c_str(), s2 ? s2->name.c_str() : "?");
                eventsOut.push_back(buf);
            }
        });
    }
}

// ---------------------------------------------------------------------------
// Money detection
// ---------------------------------------------------------------------------

void Economy::detectMoney(uint64_t tick, std::vector<std::string>& eventsOut) {
    if (!m_barter) return;

    const double threshold = static_cast<double>(cfg().getF("econ.moneyness_threshold", 0.55f));
    const uint32_t needPasses =
        static_cast<uint32_t>(cfg().getInt("econ.moneyness_passes", 3));
    const uint64_t minTrades =
        static_cast<uint64_t>(cfg().getInt("econ.money_min_trades", 200));
    const size_t minGoods =
        static_cast<size_t>(cfg().getInt("econ.money_min_goods", 6));
    const uint64_t minTotal =
        static_cast<uint64_t>(cfg().getInt("econ.money_min_total_trades", 2000));

    uint64_t grandTotal = 0;
    size_t circulating = 0;
    for (const GoodStats& g : m_goods) {
        grandTotal += g.trades;
        if (g.trades > 0) ++circulating;
    }
    if (grandTotal == 0) return;

    for (GoodStats& g : m_goods) {
        // Three factors, all of which money has and no consumable has.
        //
        //   turnover  -- what share of all trading it accounts for. Money is the
        //                thing on one side of most transactions.
        //   passRate  -- of the times someone took it, how often did they pass it
        //                on rather than use it. THE defining property: money is
        //                accepted by people who do not want it for itself.
        //   breadth   -- how many DIFFERENT goods it is accepted against. This
        //                started out as a refusal rate, which was wrong: every
        //                failed pairing charges a refusal against every good
        //                both parties held, so the measure collapsed to zero for
        //                everything and dragged the whole index down with it.
        //                Breadth is the honest version of the same idea -- money
        //                is accepted against everything, a commodity against the
        //                two or three things its users happen to want.
        const double turnover = static_cast<double>(g.trades) /
                                static_cast<double>(grandTotal);
        const double passRate = g.acquiredByTrade
            ? static_cast<double>(g.passedOn) / static_cast<double>(g.acquiredByTrade)
            : 0.0;
        const double breadth = circulating > 1
            ? static_cast<double>(g.counterGoods.size()) /
              static_cast<double>(circulating - 1)
            : 0.0;

        // Geometric-ish rather than a sum, because a good has to have ALL three
        // to be money. A widely traded good nobody passes on is a commodity, and
        // a good one person hoards and re-trades is not a currency.
        const double m = std::pow(std::max(1e-9, turnover), 0.34) *
                         std::pow(std::max(1e-9, passRate), 0.33) *
                         std::pow(std::max(1e-9, breadth), 0.33);
        g.moneyness = std::min(1.0, m);

        if (g.moneyness >= threshold && g.trades >= minTrades) ++g.sustainedPasses;
        else g.sustainedPasses = 0;
    }

    if (m_currency != 0) return;   // already have one; nothing to detect

    // Do not declare money on a thin economy. Early on only two or three goods
    // are moving at all, so whatever is being traded looks dominant and looks
    // universally accepted -- and the first thing to cross the line locks in and
    // shuts the detector down. That is a measurement artefact rather than a
    // network effect, and it was picking a good that later accounted for 3% of
    // trade while another accounted for 99%. So the detector waits until there
    // is enough breadth and volume for the three factors to mean anything.
    if (circulating < minGoods || grandTotal < minTotal) return;

    // The best candidate that has held the property long enough.
    GoodStats* best = nullptr;
    for (GoodStats& g : m_goods) {
        if (g.sustainedPasses < needPasses) continue;
        if (!best || g.moneyness > best->moneyness) best = &g;
    }
    if (!best) return;

    m_currency = best->substance;
    m_currencyEmergent = true;
    m_currencyTick = tick;
    const Substance* s = chem().substance(best->substance);
    m_currencyName = s ? s->commonName : std::string();
    if (m_currencyName.empty()) m_currencyName = s ? s->name : std::string("unit");

    char buf[420];
    std::snprintf(buf, sizeof buf,
                  "MONEY HAS EMERGED. %s is now being accepted by individuals who have no use "
                  "for it, because they know it can be passed on. Nobody decreed this: its "
                  "moneyness index is %.2f, it is accepted against %zu other goods, and it is "
                  "passed on %.0f%% of the times it is taken.",
                  m_currencyName.c_str(), best->moneyness, best->counterGoods.size(),
                  best->acquiredByTrade
                      ? 100.0 * static_cast<double>(best->passedOn) /
                        static_cast<double>(best->acquiredByTrade)
                      : 0.0);
    eventsOut.push_back(buf);
}

// ---------------------------------------------------------------------------
// Prices and distribution
// ---------------------------------------------------------------------------

double Economy::price(uint16_t substance) const {
    if (m_currency == 0 || m_exchanges.empty()) return 0.0;
    // Derived from actual exchanges against the currency, never assigned. A good
    // that has never been traded for money has no price, and the UI says so
    // rather than showing a zero that looks like "free".
    double sum = 0.0;
    uint32_t n = 0;
    for (const ExchangeRecord& r : m_exchanges) {
        if (r.tick == 0) continue;
        if (r.goodA == substance && r.goodB == m_currency && r.amountA > 1e-6f) {
            sum += static_cast<double>(r.amountB) / static_cast<double>(r.amountA);
            ++n;
        } else if (r.goodB == substance && r.goodA == m_currency && r.amountB > 1e-6f) {
            sum += static_cast<double>(r.amountA) / static_cast<double>(r.amountB);
            ++n;
        }
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

double Economy::giniCoefficient(const Agents& agents) const {
    if (m_currency == 0 && m_currencyHeld.empty()) return -1.0;
    std::vector<float> v;
    if (m_currency != 0) {
        // Commodity money: read it out of the inventories where it actually is.
        for (uint32_t slot : agents.liveSlots())
            v.push_back(agents.inventory().amountOf(slot, m_currency));
    } else {
        for (float f : m_currencyHeld) if (f > 0.0f) v.push_back(f);
    }
    if (v.size() < 2) return -1.0;
    std::sort(v.begin(), v.end());
    double sum = 0.0, weighted = 0.0;
    for (size_t i = 0; i < v.size(); ++i) {
        sum += static_cast<double>(v[i]);
        weighted += static_cast<double>(i + 1) * static_cast<double>(v[i]);
    }
    if (sum <= 0.0) return 0.0;
    const double n = static_cast<double>(v.size());
    return (2.0 * weighted) / (n * sum) - (n + 1.0) / n;
}

// ---------------------------------------------------------------------------
// Serialization. Written even when inactive, as a single zero byte, so the chunk
// layout does not change depending on whether an economy exists.
// ---------------------------------------------------------------------------

void Economy::serialize(BinaryWriter& w) const {
    const uint8_t flags = static_cast<uint8_t>((m_active ? 1u : 0u) | (m_barter ? 2u : 0u) |
                                               (m_currencyEmergent ? 4u : 0u));
    w.pod(flags);
    if (!m_active) return;

    w.pod(m_currency);
    w.str(m_currencyName);
    w.pod(m_currencyTick);
    w.pod(m_totalExchanges);
    w.pod(m_totalOffers);
    const uint32_t sc = static_cast<uint32_t>(m_substanceCount);
    w.pod(sc);

    const uint32_t gc = static_cast<uint32_t>(m_goods.size());
    w.pod(gc);
    for (const GoodStats& g : m_goods) {
        w.pod(g.substance); w.pod(g.acquiredByTrade);
        w.pod(g.passedOn); w.pod(g.timesOffered); w.pod(g.timesRefused);
        const uint32_t cgn = static_cast<uint32_t>(g.counterGoods.size());
        w.pod(cgn);
        for (uint16_t cg : g.counterGoods) w.pod(cg);
        w.pod(g.moneyness); w.pod(g.sustainedPasses);
        w.pod(g.meanRate); w.pod(g.trades);
    }

    const uint32_t ec = static_cast<uint32_t>(m_exchanges.size());
    w.pod(ec);
    const uint32_t head = static_cast<uint32_t>(m_exchangeHead);
    w.pod(head);
    for (const ExchangeRecord& r : m_exchanges) {
        w.pod(r.tick); w.pod(r.uidA); w.pod(r.uidB);
        w.pod(r.goodA); w.pod(r.goodB);
        w.pod(r.amountA); w.pod(r.amountB); w.pod(r.currencyPaid); w.pod(r.rate);
    }

    const uint32_t cc = static_cast<uint32_t>(m_currencyHeld.size());
    w.pod(cc);
    for (float f : m_currencyHeld) w.pod(f);
    const uint32_t tc = static_cast<uint32_t>(m_tookInTrade.size());
    w.pod(tc);
    for (uint8_t b : m_tookInTrade) w.pod(b);
}

void Economy::deserialize(BinaryReader& r) {
    deactivate();
    uint8_t flags = 0;
    r.pod(flags);
    if ((flags & 1u) == 0) return;   // the save had no economy, and neither do we

    m_active = true;
    m_barter = (flags & 2u) != 0;
    m_currencyEmergent = (flags & 4u) != 0;

    r.pod(m_currency);
    r.str(m_currencyName);
    r.pod(m_currencyTick);
    r.pod(m_totalExchanges);
    r.pod(m_totalOffers);
    uint32_t sc = 0;
    r.pod(sc);
    m_substanceCount = sc;

    uint32_t gc = 0;
    r.pod(gc);
    if (gc > 100000u) return;
    for (uint32_t i = 0; i < gc && r.ok(); ++i) {
        GoodStats g;
        r.pod(g.substance); r.pod(g.acquiredByTrade);
        r.pod(g.passedOn); r.pod(g.timesOffered); r.pod(g.timesRefused);
        uint32_t cgn = 0;
        r.pod(cgn);
        if (cgn > 100000u) return;
        for (uint32_t k = 0; k < cgn && r.ok(); ++k) {
            uint16_t cg = 0;
            r.pod(cg);
            g.counterGoods.push_back(cg);
        }
        r.pod(g.moneyness); r.pod(g.sustainedPasses);
        r.pod(g.meanRate); r.pod(g.trades);
        m_goods.push_back(g);
    }

    uint32_t ec = 0;
    r.pod(ec);
    if (ec > 1000000u) return;
    uint32_t head = 0;
    r.pod(head);
    m_exchangeHead = head;
    m_exchanges.clear();
    for (uint32_t i = 0; i < ec && r.ok(); ++i) {
        ExchangeRecord rec;
        r.pod(rec.tick); r.pod(rec.uidA); r.pod(rec.uidB);
        r.pod(rec.goodA); r.pod(rec.goodB);
        r.pod(rec.amountA); r.pod(rec.amountB); r.pod(rec.currencyPaid); r.pod(rec.rate);
        m_exchanges.push_back(rec);
    }
    if (m_exchangeHead >= m_exchanges.size()) m_exchangeHead = 0;

    uint32_t cc = 0;
    r.pod(cc);
    if (cc > 10000000u) return;
    m_currencyHeld.resize(cc);
    for (uint32_t i = 0; i < cc && r.ok(); ++i) r.pod(m_currencyHeld[i]);
    uint32_t tc = 0;
    r.pod(tc);
    if (tc > 100000000u) return;
    m_tookInTrade.resize(tc);
    for (uint32_t i = 0; i < tc && r.ok(); ++i) r.pod(m_tookInTrade[i]);
}

}  // namespace gen
