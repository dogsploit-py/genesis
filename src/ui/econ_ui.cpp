// ui/econ_ui.cpp — the Economy panel.
//
// Most of the time this panel's job is to say, accurately, that there is no
// economy. That is not a stub: it is the state the world is actually in, and the
// panel is explicit about the difference between "no money exists" and "money
// exists but everyone has zero", because they are not the same thing and the
// second one would be a lie about this program.
//
// Once barter is switched on, the interesting number is the coincidence rate:
// what fraction of encounters between two willing traders actually produced an
// exchange. It starts low, because barter needs a double coincidence of wants,
// and that failure rate is the entire reason money is worth inventing.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "chem/reactions.h"
#include "core/config.h"
#include "imgui.h"
#include "sim/time.h"
#include "ui/app.h"

namespace gen {

namespace {

void drawDormant(Simulation& sim, UiState& ui) {
    ImGui::TextWrapped("There is no economy in this world.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Not \"money exists but nobody has any\" -- the concept is absent. There are no prices, "
        "no valuations in currency units, no market structures allocated, and no wealth field on "
        "any individual. The economy stage is not entered: the tick's guard is a single bool "
        "test at the call site, so no function in econ/economy.cpp has executed a single "
        "instruction this run.");
    ImGui::Spacing();
    ImGui::TextDisabled(
        "That is a structural property, not a promise. Every container in the module is empty "
        "and unallocated until one of the buttons below is pressed, and deleting the module "
        "would leave the rest of the program complete.");

    ImGui::SeparatorText("Bring one into existence");
    ImGui::TextWrapped(
        "Two routes, and they are not the same. Enabling barter gives nobody anything: it makes "
        "exchange POSSIBLE and leaves it to the inhabitants, and if a medium of exchange emerges "
        "from that it will be because a good started being accepted by individuals who had no "
        "use for it. Decreeing a currency skips all of that.");

    ImGui::Spacing();
    if (ImGui::Button("Enable barter", ImVec2(210, 0))) {
        GodAction a;
        a.kind = GodActionKind::EnableBarter;
        castGodAction(sim, ui, a);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Agents may exchange goods they are carrying, when both value what the "
                          "other holds more than what they hold themselves. Nothing is given to "
                          "anyone.");

    ImGui::Spacing();
    ImGui::SeparatorText("Or decree it");
    ImGui::TextWrapped(
        "A currency by fiat. Honest about what it is: an imposition rather than a discovery, and "
        "the event log will record it that way.");

    // Substance picker: a real commodity currency, or a pure token.
    static std::vector<uint16_t> ids;
    static std::vector<std::string> labels;
    if (labels.empty()) {
        ids.push_back(0);
        labels.push_back("(fiat token -- backed by nothing)");
        for (const Substance& s : chem().substances()) {
            if (s.phase != Phase::Solid || s.nuclear) continue;
            ids.push_back(s.id);
            labels.push_back(s.formulaText + "  " + s.name);
        }
    }
    if (ui.econCurrencyIndex < 0 || ui.econCurrencyIndex >= static_cast<int>(ids.size()))
        ui.econCurrencyIndex = 0;
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::BeginCombo("Backed by",
                          labels[static_cast<size_t>(ui.econCurrencyIndex)].c_str())) {
        for (int i = 0; i < static_cast<int>(ids.size()); ++i)
            if (ImGui::Selectable(labels[static_cast<size_t>(i)].c_str(),
                                  i == ui.econCurrencyIndex))
                ui.econCurrencyIndex = i;
        ImGui::EndCombo();
    }
    helpMarker("A commodity currency is a real substance that agents can also use, so its value "
               "is anchored to what it is good for. A fiat token is worth something only because "
               "everyone accepts it -- which is a much more fragile arrangement, and the "
               "simulation will not pretend otherwise.");

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Name", ui.econCurrencyName, sizeof ui.econCurrencyName);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Initial holding each", &ui.econInitialHolding, 0.0f, 1000.0f, "%.0f");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.30f, 0.12f, 1.0f));
    if (ImGui::Button("INTRODUCE CURRENCY", ImVec2(210, 0))) {
        GodAction a;
        a.kind = GodActionKind::IntroduceCurrency;
        a.i0 = static_cast<int32_t>(ids[static_cast<size_t>(ui.econCurrencyIndex)]);
        a.f0 = ui.econInitialHolding;
        a.text = ui.econCurrencyName[0] ? ui.econCurrencyName : "unit";
        castGodAction(sim, ui, a);
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled(
        "The same rule applies to property law, formal government, organised religion, written "
        "language, contracts, taxation and slavery. None are baseline features. Each either "
        "arises from agent behaviour and gets detected and logged, or gets imposed from here.");
}

void drawActive(Simulation& sim, UiState& ui) {
    struct Snap {
        bool hasCurrency = false;
        std::string currencyName;
        uint16_t currency = 0;
        uint64_t trades = 0, offers = 0;
        double coincidence = 0.0;
        double gini = 0.0;
        std::vector<GoodStats> goods;
        std::vector<ExchangeRecord> recent;
        std::vector<double> prices;
    };
    static Snap snap;
    {
        const Economy& e = sim.economy();
        snap.hasCurrency = e.hasCurrency();
        snap.currencyName = e.currencyName();
        snap.currency = e.currency();
        snap.trades = e.totalExchanges();
        snap.offers = e.totalOffers();
        snap.coincidence = e.coincidenceRate();
        sim.readAgents([&](const Agents& a) { snap.gini = e.giniCoefficient(a); });
        snap.goods = e.goods();
        snap.recent.clear();
        for (const ExchangeRecord& r : e.exchanges())
            if (r.tick != 0) snap.recent.push_back(r);
        std::sort(snap.recent.begin(), snap.recent.end(),
                  [](const ExchangeRecord& a, const ExchangeRecord& b) { return a.tick > b.tick; });
        if (snap.recent.size() > 200) snap.recent.resize(200);
        snap.prices.assign(snap.goods.size(), 0.0);
        for (size_t i = 0; i < snap.goods.size(); ++i)
            snap.prices[i] = e.price(snap.goods[i].substance);
    }

    if (snap.hasCurrency) {
        ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.40f, 1.0f), "Currency: %s",
                           snap.currencyName.c_str());
        ImGui::SameLine();
        if (snap.gini >= 0.0) ImGui::TextDisabled("|  wealth Gini %.3f", snap.gini);
        else ImGui::TextDisabled("|  too few holders to measure inequality");
    } else {
        ImGui::TextUnformatted("Barter only. No currency has emerged and none has been decreed.");
    }

    ImGui::Text("%llu exchanges from %llu willing encounters",
                static_cast<unsigned long long>(snap.trades),
                static_cast<unsigned long long>(snap.offers));
    ImGui::SameLine();
    ImGui::TextDisabled("-- a %.1f%% coincidence rate", snap.coincidence * 100.0);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "That percentage is the whole problem with barter. Two individuals both willing to trade "
        "still need a double coincidence of wants, and most of the time they do not have one. "
        "The lower this number sits, the more a medium of exchange would be worth to them.");
    ImGui::PopTextWrapPos();

    ImGui::SeparatorText("Goods");
    ImGui::TextDisabled(
        "Moneyness combines three things money has and no consumable has: share of all trading, "
        "how often a taker passes it on instead of using it, and how many DIFFERENT goods it is "
        "accepted against. Multiplied rather than added, because a good needs all three -- a "
        "heavily traded good nobody passes on is a commodity, and a good that two people keep "
        "swapping with each other is not a currency.");

    if (snap.goods.empty()) {
        ImGui::TextDisabled("Nothing has been traded yet.");
    } else if (ImGui::BeginTable("goods", 7,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                                 ImVec2(0, 210.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Good");
        ImGui::TableSetupColumn("Trades", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Taken", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Passed on", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Breadth", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Moneyness", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        std::vector<size_t> order(snap.goods.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return snap.goods[a].moneyness > snap.goods[b].moneyness;
        });

        for (size_t oi : order) {
            const GoodStats& g = snap.goods[oi];
            const Substance* s = chem().substance(g.substance);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (g.substance == snap.currency && snap.hasCurrency)
                ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.40f, 1.0f), "%s  <- currency",
                                   s ? s->name.c_str() : "?");
            else
                ImGui::TextUnformatted(s ? s->name.c_str() : "?");
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(g.trades));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(g.acquiredByTrade));
            ImGui::TableNextColumn();
            if (g.acquiredByTrade)
                ImGui::Text("%.0f%%", 100.0 * static_cast<double>(g.passedOn) /
                                      static_cast<double>(g.acquiredByTrade));
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            if (!g.counterGoods.empty())
                ImGui::Text("%zu goods", g.counterGoods.size());
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.3f", g.moneyness);
                ImGui::ProgressBar(static_cast<float>(g.moneyness), ImVec2(-1, 0), buf);
            }
            ImGui::TableNextColumn();
            if (snap.hasCurrency && snap.prices[oi] > 0.0)
                ImGui::Text("%.3f", snap.prices[oi]);
            else if (snap.hasCurrency)
                ImGui::TextDisabled("never sold");
            else
                ImGui::TextDisabled("no money");
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Recent exchanges");
    if (snap.recent.empty()) {
        ImGui::TextDisabled("None yet.");
    } else if (ImGui::BeginTable("trades", 4,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY, ImVec2(0, 170.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Year", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Gave");
        ImGui::TableSetupColumn("Received");
        ImGui::TableSetupColumn("Rate", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const ExchangeRecord& r : snap.recent) {
            const Substance* sa = chem().substance(r.goodA);
            const Substance* sb = chem().substance(r.goodB);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<double>(r.tick) / kHoursPerYear);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f %s", static_cast<double>(r.amountA), sa ? sa->formulaText.c_str() : "?");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f %s", static_cast<double>(r.amountB), sb ? sb->formulaText.c_str() : "?");
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", static_cast<double>(r.rate));
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (!snap.hasCurrency) {
        ImGui::TextDisabled("If a good crosses the moneyness threshold and holds it, the "
                            "simulation will say so. It may never happen, and this panel will "
                            "not invent a currency to have something to show.");
        ImGui::Spacing();
        if (ImGui::Button("Decree one instead", ImVec2(190, 0))) ui.econShowDecree = true;
        if (ui.econShowDecree) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputText("Name##2", ui.econCurrencyName, sizeof ui.econCurrencyName);
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat("Each##2", &ui.econInitialHolding, 0.0f, 1000.0f, "%.0f");
            if (ImGui::Button("INTRODUCE CURRENCY##2", ImVec2(210, 0))) {
                GodAction a;
                a.kind = GodActionKind::IntroduceCurrency;
                a.i0 = 0;
                a.f0 = ui.econInitialHolding;
                a.text = ui.econCurrencyName[0] ? ui.econCurrencyName : "unit";
                castGodAction(sim, ui, a);
                ui.econShowDecree = false;
            }
            ImGui::Unindent();
        }
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.14f, 0.14f, 1.0f));
    if (ImGui::Button("Abolish the economy", ImVec2(190, 0))) {
        GodAction a;
        a.kind = GodActionKind::AbolishEconomy;
        castGodAction(sim, ui, a);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("Releases every structure and returns the world to having no economy at "
                        "all. Not undoable through Ctrl+Z -- undo restores tiles and agents, and "
                        "this touches neither.");
}

}  // namespace

void drawEconomy(Simulation& sim, UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Economy", &ui.showEconomy)) { ImGui::End(); return; }

    if (sim.economy().active()) drawActive(sim, ui);
    else drawDormant(sim, ui);

    ImGui::End();
}

}  // namespace gen
