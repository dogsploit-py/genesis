#include "sim/brain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/config.h"
#include "core/serialize.h"

namespace gen {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char* brainInputName(int i) {
    static const char* kNames[kBrainInputCount] = {
        "vision.plant[0]", "vision.plant[1]", "vision.plant[2]",
        "vision.plant[3]", "vision.plant[4]", "vision.plant[5]",
        "vision.agent[0]", "vision.agent[1]", "vision.agent[2]",
        "vision.agent[3]", "vision.agent[4]", "vision.agent[5]",
        "vision.water[0]", "vision.water[1]", "vision.water[2]",
        "vision.water[3]", "vision.water[4]", "vision.water[5]",
        "self.hunger", "self.thirst", "self.energy", "self.health",
        "self.pain", "self.stress", "self.bodyTemp", "self.age",
        "self.reproReady",
        "env.light", "env.temperature", "env.terrain", "env.soilMoisture",
        "other.bearing", "other.distance", "other.sexExpression",
        "other.attraction", "other.relatedness", "other.isBonded",
        "signal[0]", "signal[1]", "signal[2]", "signal[3]",
        "memory.foodDir", "memory.waterDir",
        "recurrent[0]", "recurrent[1]", "recurrent[2]", "recurrent[3]",
        "bias",
    };
    return (i >= 0 && i < kBrainInputCount) ? kNames[i] : "?";
}

const char* brainOutputName(int i) {
    static const char* kNames[kBrainOutputCount] = {
        "turn", "accelerate", "eat", "drink", "pickUp", "drop", "use",
        "attack", "flee", "groom", "courtshipDisplay", "acceptMating",
        "vocalise[0]", "vocalise[1]", "vocalise[2]", "vocalise[3]",
        "teach", "share", "build", "rest",
    };
    return (i >= 0 && i < kBrainOutputCount) ? kNames[i] : "?";
}

const char* driveName(Drive d) {
    switch (d) {
        case Drive::Hunger:           return "Hunger";
        case Drive::Thirst:           return "Thirst";
        case Drive::Thermoregulation: return "Thermoregulation";
        case Drive::Safety:           return "Safety";
        case Drive::Rest:             return "Rest";
        case Drive::Social:           return "Social";
        case Drive::Curiosity:        return "Curiosity";
        case Drive::Reproduction:     return "Reproduction";
        case Drive::Count:            break;
    }
    return "?";
}

Trait driveRewardTrait(Drive d) {
    switch (d) {
        case Drive::Hunger:           return Trait::RewardHunger;
        case Drive::Thirst:           return Trait::RewardThirst;
        case Drive::Thermoregulation: return Trait::RewardThermal;
        case Drive::Safety:           return Trait::RewardSafety;
        case Drive::Rest:             return Trait::RewardRest;
        case Drive::Social:           return Trait::RewardSocial;
        case Drive::Curiosity:        return Trait::RewardCuriosity;
        case Drive::Reproduction:     return Trait::RewardRepro;
        case Drive::Count:            break;
    }
    return Trait::RewardHunger;
}

const char* activationName(Activation a) {
    switch (a) {
        case Activation::Tanh:     return "tanh";
        case Activation::Sigmoid:  return "sigmoid";
        case Activation::Relu:     return "relu";
        case Activation::Gaussian: return "gaussian";
        case Activation::Sine:     return "sine";
        case Activation::Linear:   return "linear";
        case Activation::Count:    break;
    }
    return "?";
}

namespace {
inline float activate(Activation a, float x) {
    switch (a) {
        case Activation::Tanh:     return std::tanh(x);
        case Activation::Sigmoid:  return 1.0f / (1.0f + std::exp(-x));
        case Activation::Relu:     return (x > 0.0f) ? x : 0.0f;
        case Activation::Gaussian: return std::exp(-x * x);
        case Activation::Sine:     return std::sin(x);
        case Activation::Linear:   return x;
        case Activation::Count:    break;
    }
    return std::tanh(x);
}
}  // namespace

int Brains::nodeIndexOf(const NodeGene* nodes, uint16_t count, uint32_t id) {
    // The node array is kept sorted by id, so this is a binary search rather
    // than the linear scan the naive version would need.
    uint16_t lo = 0, hi = count;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (nodes[mid].id < id) lo = static_cast<uint16_t>(mid + 1);
        else hi = mid;
    }
    return (lo < count && nodes[lo].id == id) ? static_cast<int>(lo) : -1;
}

// ---------------------------------------------------------------------------

void Brains::configure(size_t maxAgents, uint16_t nodeCapacity, uint16_t connCapacity) {
    m_slotCount = maxAgents;
    m_nodeCapacity = std::max<uint16_t>(nodeCapacity,
        static_cast<uint16_t>(kBrainInputCount + kBrainOutputCount + 2));
    m_connCapacity = connCapacity;

    m_nodes.assign(maxAgents * m_nodeCapacity, NodeGene());
    m_conns.assign(maxAgents * m_connCapacity, ConnGene());
    m_nodeCount.assign(maxAgents, 0);
    m_connCount.assign(maxAgents, 0);

    m_activation.assign(maxAgents * m_nodeCapacity, 0.0f);
    m_previous.assign(maxAgents * m_nodeCapacity, 0.0f);
    m_bias.assign(maxAgents * m_nodeCapacity, 0.0f);
    m_order.assign(maxAgents * m_nodeCapacity, 0);
    m_inStart.assign(maxAgents * (static_cast<size_t>(m_nodeCapacity) + 1), 0);
    m_weight.assign(maxAgents * m_connCapacity, 0.0f);
    m_eligibility.assign(maxAgents * m_connCapacity, 0.0f);
    m_connSrc.assign(maxAgents * m_connCapacity, 0);
    m_connDst.assign(maxAgents * m_connCapacity, 0);
    m_inList.assign(maxAgents * m_connCapacity, 0);
    m_depth.assign(m_nodeCapacity, 0);

    m_runtime.assign(maxAgents, BrainRuntime());
    const size_t inStride = static_cast<size_t>(m_nodeCapacity) + 1;
    for (size_t s = 0; s < maxAgents; ++s) {
        BrainRuntime& rt = m_runtime[s];
        rt.activation  = &m_activation[s * m_nodeCapacity];
        rt.previous    = &m_previous[s * m_nodeCapacity];
        rt.bias        = &m_bias[s * m_nodeCapacity];
        rt.order       = &m_order[s * m_nodeCapacity];
        rt.weight      = &m_weight[s * m_connCapacity];
        rt.eligibility = &m_eligibility[s * m_connCapacity];
        rt.connSrc     = &m_connSrc[s * m_connCapacity];
        rt.connDst     = &m_connDst[s * m_connCapacity];
        rt.inList      = &m_inList[s * m_connCapacity];
        rt.inStart     = &m_inStart[s * inStride];
    }
    // Hidden node ids and connection innovations share one counter, starting
    // above the fixed input/output ids so they can never collide.
    m_nextInnovation = static_cast<uint32_t>(kBrainInputCount + kBrainOutputCount + 1);
}

BrainView Brains::brain(size_t slot) {
    BrainView v;
    v.nodes = &m_nodes[slot * m_nodeCapacity];
    v.conns = &m_conns[slot * m_connCapacity];
    v.nodeCount = m_nodeCount[slot];
    v.connCount = m_connCount[slot];
    v.nodeCapacity = m_nodeCapacity;
    v.connCapacity = m_connCapacity;
    return v;
}

ConstBrainView Brains::brain(size_t slot) const {
    ConstBrainView v;
    v.nodes = &m_nodes[slot * m_nodeCapacity];
    v.conns = &m_conns[slot * m_connCapacity];
    v.nodeCount = m_nodeCount[slot];
    v.connCount = m_connCount[slot];
    return v;
}

void Brains::sortNodes(size_t slot) {
    NodeGene* nodes = &m_nodes[slot * m_nodeCapacity];
    std::sort(nodes, nodes + m_nodeCount[slot],
              [](const NodeGene& a, const NodeGene& b) { return a.id < b.id; });
}

void Brains::makeFounder(size_t slot, Rng& rng) {
    BrainView b = brain(slot);
    uint16_t n = 0;

    // Ids 0..kBrainInputCount-1 are inputs and the next block is outputs. These
    // are fixed forever, which is what lets any two brains be aligned, and --
    // combined with the sorted-by-id invariant -- means inputs and outputs
    // always sit at known indices.
    for (int i = 0; i < kBrainInputCount; ++i) {
        NodeGene g;
        g.id = static_cast<uint32_t>(i);
        g.kind = static_cast<uint8_t>(NodeKind::Input);
        g.activation = static_cast<uint8_t>(Activation::Linear);
        b.nodes[n++] = g;
    }
    for (int i = 0; i < kBrainOutputCount; ++i) {
        NodeGene g;
        g.id = static_cast<uint32_t>(kBrainInputCount + i);
        g.kind = static_cast<uint8_t>(NodeKind::Output);
        g.activation = static_cast<uint8_t>(Activation::Tanh);
        g.biasA = rng.gaussian(0.0f, 0.3f);
        g.biasB = rng.gaussian(0.0f, 0.3f);
        b.nodes[n++] = g;
    }
    m_nodeCount[slot] = n;
    m_connCount[slot] = 0;

    // Sparse initial wiring. A fully connected 48x20 start would be 960
    // connections of pure noise; starting sparse means useful structure has to
    // be found, which is the point of letting topology evolve at all.
    const float density = cfg().getF("brain.founder_connection_density", 0.12f);
    const float plasticFraction = cfg().getF("brain.plastic_fraction", 0.45f);
    for (int i = 0; i < kBrainInputCount; ++i) {
        for (int o = 0; o < kBrainOutputCount; ++o) {
            if (rng.nextFloat() >= density) continue;
            if (m_connCount[slot] >= m_connCapacity) break;
            ConnGene c;
            c.from = static_cast<uint32_t>(i);
            c.to = static_cast<uint32_t>(kBrainInputCount + o);
            c.innovation = m_nextInnovation++;
            c.flags = ConnFlag_Enabled;
            if (rng.nextFloat() < plasticFraction) c.flags |= ConnFlag_Plastic;
            c.dominance = static_cast<uint8_t>(rng.nextFloat() < 0.75f
                                               ? Dominance::Additive : Dominance::CompleteDominant);
            c.weightA = rng.gaussian(0.0f, 0.8f);
            c.weightB = rng.gaussian(0.0f, 0.8f);
            c.plasticity = rng.rangef(0.2f, 1.0f);
            b.conns[m_connCount[slot]++] = c;
        }
    }

    if (cfg().getBool("brain.innate_reflexes", true)) applyInnateReflexes(slot, rng);
}

void Brains::setConnection(size_t slot, uint32_t from, uint32_t to, float weight, Rng& rng) {
    BrainView b = brain(slot);
    for (uint16_t i = 0; i < m_connCount[slot]; ++i) {
        if (b.conns[i].from != from || b.conns[i].to != to) continue;
        b.conns[i].weightA = weight + rng.gaussian(0.0f, 0.15f);
        b.conns[i].weightB = weight + rng.gaussian(0.0f, 0.15f);
        b.conns[i].flags |= ConnFlag_Enabled;
        return;
    }
    addConnection(slot, from, to, weight, rng);
    // addConnection jitters allele B only; set both around the target so the
    // expressed weight starts where the reflex intends.
    if (m_connCount[slot] > 0) {
        ConnGene& c = b.conns[m_connCount[slot] - 1];
        if (c.from == from && c.to == to) {
            c.weightA = weight + rng.gaussian(0.0f, 0.15f);
            c.weightB = weight + rng.gaussian(0.0f, 0.15f);
        }
    }
}

void Brains::applyInnateReflexes(size_t slot, Rng& rng) {
    const float s = cfg().getF("brain.innate_reflex_strength", 2.5f);

    // Consummatory reflexes: act on the resource you are standing in.
    setConnection(slot, In_Thirst, kBrainInputCount + Out_Drink, s, rng);
    setConnection(slot, In_Hunger, kBrainInputCount + Out_Eat, s, rng);

    // Appetitive reflexes: go looking when a deficit is felt.
    setConnection(slot, In_Hunger, kBrainInputCount + Out_Accelerate, s * 0.35f, rng);
    setConnection(slot, In_Thirst, kBrainInputCount + Out_Accelerate, s * 0.35f, rng);

    // Taxis. Sectors 2 and 3 look forward, 0/1 to one side and 4/5 to the
    // other, so a positive weight to Accelerate on the forward sectors and a
    // signed weight to Turn on the flanks produces approach behaviour.
    for (int k = 0; k < 6; ++k) {
        const float forward = (k == 2 || k == 3) ? 1.0f : 0.0f;
        // Sectors 0..2 sit at negative relative bearing, 3..5 at positive.
        const float turn = (k < 3) ? -1.0f : 1.0f;
        const float flank = (k == 0 || k == 5) ? 0.9f : (k == 1 || k == 4) ? 0.7f : 0.25f;

        setConnection(slot, In_VisionPlant0 + k, kBrainInputCount + Out_Accelerate,
                      s * 0.30f * forward, rng);
        setConnection(slot, In_VisionWater0 + k, kBrainInputCount + Out_Accelerate,
                      s * 0.30f * forward, rng);
        setConnection(slot, In_VisionPlant0 + k, kBrainInputCount + Out_Turn,
                      s * 0.22f * turn * flank, rng);
        setConnection(slot, In_VisionWater0 + k, kBrainInputCount + Out_Turn,
                      s * 0.22f * turn * flank, rng);
    }

    // Homing on remembered resources. The memory inputs carry the SIGNED
    // relative bearing to the last place this agent successfully ate or drank,
    // so a positive weight into Turn steers back toward it. Without this the
    // sensory memory exists but nothing uses it, and an agent that wanders off
    // a good patch has no way back -- which is what most starvation actually
    // was. It also produces home-range behaviour, which is what real foragers do.
    setConnection(slot, In_MemoryFoodDirection, kBrainInputCount + Out_Turn, s * 0.45f, rng);
    setConnection(slot, In_MemoryWaterDirection, kBrainInputCount + Out_Turn, s * 0.35f, rng);

    // Reproductive reflexes: display and accept when ready. Without these the
    // very first generation never mates and there is no second generation to
    // select on.
    setConnection(slot, In_ReproductiveReadiness, kBrainInputCount + Out_CourtshipDisplay, s, rng);
    setConnection(slot, In_ReproductiveReadiness, kBrainInputCount + Out_AcceptMating, s * 0.8f, rng);
    setConnection(slot, In_NearestAttraction, kBrainInputCount + Out_AcceptMating, s * 0.5f, rng);
    setConnection(slot, In_NearestAttraction, kBrainInputCount + Out_Accelerate, s * 0.25f, rng);

    // Aversive reflexes.
    setConnection(slot, In_Pain, kBrainInputCount + Out_Flee, s * 0.7f, rng);
    setConnection(slot, In_Stress, kBrainInputCount + Out_Flee, s * 0.5f, rng);

    // Costly actions start INHIBITED. With random weights and a zero bias,
    // attack and share both fire about half the time for no reason: unprovoked
    // aggression killed roughly a third of every founding population, and
    // reflexive sharing drained agents to their reserve floor. A negative
    // resting bias means these need positive evidence before they fire. The
    // bias is an ordinary heritable allele, so selection can undo it.
    {
        BrainView b = brain(slot);
        auto biasOutput = [&](int output, float bias) {
            const int idx = nodeIndexOf(b.nodes, m_nodeCount[slot],
                                        static_cast<uint32_t>(kBrainInputCount + output));
            if (idx < 0) return;
            b.nodes[idx].biasA = bias + rng.gaussian(0.0f, 0.15f);
            b.nodes[idx].biasB = bias + rng.gaussian(0.0f, 0.15f);
        };
        biasOutput(Out_Attack, -cfg().getF("brain.aggression_bias", 2.0f));
        biasOutput(Out_Share, -cfg().getF("brain.sharing_bias", 1.2f));
    }

    // A resting bias when energy is low, so an exhausted agent stops burning
    // what it has left on movement.
    setConnection(slot, In_Energy, kBrainInputCount + Out_Accelerate, s * 0.3f, rng);

    // Sort by innovation so the crossover merge stays linear.
    ConnGene* conns = &m_conns[slot * m_connCapacity];
    std::sort(conns, conns + m_connCount[slot],
              [](const ConnGene& a, const ConnGene& b) { return a.innovation < b.innovation; });
}

void Brains::resetBrain(size_t slot, Rng& rng) {
    makeFounder(slot, rng);
    BrainRuntime& rt = m_runtime[slot];
    for (uint16_t i = 0; i < m_nodeCapacity; ++i) { rt.activation[i] = 0.0f; rt.previous[i] = 0.0f; }
    for (uint16_t i = 0; i < m_connCapacity; ++i) rt.eligibility[i] = 0.0f;
    rt.valueBaseline = 0.0f;
}

void Brains::copyBrain(size_t dstSlot, size_t srcSlot) {
    std::memcpy(&m_nodes[dstSlot * m_nodeCapacity], &m_nodes[srcSlot * m_nodeCapacity],
                sizeof(NodeGene) * m_nodeCapacity);
    std::memcpy(&m_conns[dstSlot * m_connCapacity], &m_conns[srcSlot * m_connCapacity],
                sizeof(ConnGene) * m_connCapacity);
    std::memcpy(&m_weight[dstSlot * m_connCapacity], &m_weight[srcSlot * m_connCapacity],
                sizeof(float) * m_connCapacity);
    m_nodeCount[dstSlot] = m_nodeCount[srcSlot];
    m_connCount[dstSlot] = m_connCount[srcSlot];
}

// ---------------------------------------------------------------------------
// Inheritance
// ---------------------------------------------------------------------------

void Brains::inherit(size_t childSlot, size_t motherSlot, size_t fatherSlot,
                     Rng& rng, float mutationRateMultiplier) {
    ConstBrainView mum = brain(motherSlot);
    ConstBrainView dad = brain(fatherSlot);
    BrainView child = brain(childSlot);

    // Nodes: the union of both parents' node sets, because a connection is
    // meaningless without the nodes at its ends. Bias alleles are diploid.
    uint16_t nn = 0;
    for (uint16_t i = 0; i < mum.nodeCount && nn < m_nodeCapacity; ++i) {
        NodeGene g = mum.nodes[i];
        const int j = nodeIndexOf(dad.nodes, dad.nodeCount, g.id);
        if (j >= 0) {
            g.biasB = dad.nodes[j].biasB;
            if (rng.nextFloat() < 0.5f) g.activation = dad.nodes[j].activation;
        }
        child.nodes[nn++] = g;
    }
    for (uint16_t j = 0; j < dad.nodeCount && nn < m_nodeCapacity; ++j) {
        if (nodeIndexOf(mum.nodes, mum.nodeCount, dad.nodes[j].id) >= 0) continue;
        child.nodes[nn++] = dad.nodes[j];
    }
    m_nodeCount[childSlot] = nn;
    sortNodes(childSlot);

    // Connections aligned by innovation number. Both parents' connection arrays
    // are sorted by innovation (maintained below), so this is a linear merge
    // rather than the quadratic search the obvious implementation would do.
    const float disjointInherit = cfg().getF("brain.disjoint_inherit_chance", 0.65f);
    uint16_t nc = 0;
    auto emit = [&](const ConnGene& c) { if (nc < m_connCapacity) child.conns[nc++] = c; };

    uint16_t i = 0, j = 0;
    while (i < mum.connCount || j < dad.connCount) {
        if (i < mum.connCount &&
            (j >= dad.connCount || mum.conns[i].innovation < dad.conns[j].innovation)) {
            // Disjoint/excess from the mother. Not always inherited: taking
            // every one would make topology grow monotonically and never simplify.
            if (rng.nextFloat() < disjointInherit) emit(mum.conns[i]);
            ++i;
        } else if (j < dad.connCount &&
                   (i >= mum.connCount || dad.conns[j].innovation < mum.conns[i].innovation)) {
            if (rng.nextFloat() < disjointInherit) emit(dad.conns[j]);
            ++j;
        } else {
            // Matching gene: one weight allele from each parent. THIS is what
            // makes a connection diploid rather than merely picked from a
            // parent as in standard NEAT.
            ConnGene c = mum.conns[i];
            c.weightA = mum.conns[i].weightA;
            c.weightB = dad.conns[j].weightB;
            c.plasticity = 0.5f * (mum.conns[i].plasticity + dad.conns[j].plasticity);
            const bool eitherDisabled = ((mum.conns[i].flags & ConnFlag_Enabled) == 0) ||
                                        ((dad.conns[j].flags & ConnFlag_Enabled) == 0);
            if (eitherDisabled && rng.nextFloat() < 0.75f)
                c.flags = static_cast<uint8_t>(c.flags & ~ConnFlag_Enabled);
            else
                c.flags = static_cast<uint8_t>(c.flags | ConnFlag_Enabled);
            emit(c);
            ++i;
            ++j;
        }
    }
    m_connCount[childSlot] = nc;

    const float mult = (mutationRateMultiplier > 0.0f) ? mutationRateMultiplier : 1.0f;
    mutateTopology(childSlot, rng, mult);

    // Restore both invariants the next crossover depends on.
    sortNodes(childSlot);
    ConnGene* conns = &m_conns[childSlot * m_connCapacity];
    std::sort(conns, conns + m_connCount[childSlot],
              [](const ConnGene& a, const ConnGene& b) { return a.innovation < b.innovation; });

    // A newborn starts with no eligibility traces and no reward expectation.
    BrainRuntime& rt = m_runtime[childSlot];
    for (uint16_t k = 0; k < m_connCapacity; ++k) rt.eligibility[k] = 0.0f;
    for (uint16_t k = 0; k < m_nodeCapacity; ++k) { rt.activation[k] = 0.0f; rt.previous[k] = 0.0f; }
    rt.valueBaseline = 0.0f;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

void Brains::addConnection(size_t slot, uint32_t from, uint32_t to, float weight, Rng& rng) {
    if (m_connCount[slot] >= m_connCapacity) return;
    BrainView b = brain(slot);
    for (uint16_t i = 0; i < m_connCount[slot]; ++i)
        if (b.conns[i].from == from && b.conns[i].to == to) return;  // already exists

    ConnGene c;
    c.from = from;
    c.to = to;
    c.innovation = m_nextInnovation++;
    c.flags = ConnFlag_Enabled;
    if (rng.nextFloat() < cfg().getF("brain.plastic_fraction", 0.45f)) c.flags |= ConnFlag_Plastic;
    c.dominance = static_cast<uint8_t>(Dominance::Additive);
    c.weightA = weight;
    c.weightB = weight + rng.gaussian(0.0f, 0.1f);
    c.plasticity = rng.rangef(0.2f, 1.0f);
    b.conns[m_connCount[slot]++] = c;
}

void Brains::mutateTopology(size_t slot, Rng& rng, float mult) {
    BrainView b = brain(slot);
    const float weightRate  = cfg().getF("brain.weight_mutation_rate", 0.06f) * mult;
    const float weightSigma = cfg().getF("brain.weight_mutation_sigma", 0.22f);
    const float addConnRate = cfg().getF("brain.add_connection_rate", 0.05f) * mult;
    const float addNodeRate = cfg().getF("brain.add_node_rate", 0.018f) * mult;
    const float toggleRate  = cfg().getF("brain.toggle_connection_rate", 0.012f) * mult;
    const float actRate     = cfg().getF("brain.activation_mutation_rate", 0.01f) * mult;

    for (uint16_t i = 0; i < m_connCount[slot]; ++i) {
        ConnGene& c = b.conns[i];
        if (rng.nextFloat() < weightRate) {
            float& w = (rng.nextFloat() < 0.5f) ? c.weightA : c.weightB;
            w = std::min(8.0f, std::max(-8.0f, w + rng.gaussian(0.0f, weightSigma)));
        }
        if (rng.nextFloat() < weightRate * 0.3f)
            c.plasticity = std::min(1.0f, std::max(0.0f, c.plasticity + rng.gaussian(0.0f, 0.1f)));
        if (rng.nextFloat() < toggleRate) c.flags ^= ConnFlag_Enabled;
    }
    for (uint16_t i = 0; i < m_nodeCount[slot]; ++i) {
        NodeGene& g = b.nodes[i];
        if (static_cast<NodeKind>(g.kind) == NodeKind::Input) continue;
        if (rng.nextFloat() < weightRate) {
            float& bias = (rng.nextFloat() < 0.5f) ? g.biasA : g.biasB;
            bias = std::min(8.0f, std::max(-8.0f, bias + rng.gaussian(0.0f, weightSigma)));
        }
        if (rng.nextFloat() < actRate)
            g.activation = static_cast<uint8_t>(rng.range(0, static_cast<int>(Activation::Count)));
    }

    // Add a connection between two existing nodes. Cycles are allowed on
    // purpose -- they become the recurrent edges that give the network memory.
    if (rng.nextFloat() < addConnRate && m_nodeCount[slot] > 1) {
        const uint16_t a = static_cast<uint16_t>(rng.range(0, m_nodeCount[slot]));
        const uint16_t z = static_cast<uint16_t>(rng.range(0, m_nodeCount[slot]));
        if (static_cast<NodeKind>(b.nodes[z].kind) != NodeKind::Input)
            addConnection(slot, b.nodes[a].id, b.nodes[z].id, rng.gaussian(0.0f, 0.7f), rng);
    }

    // Add a node by splitting a connection: the old edge is disabled and
    // replaced by two, the first weighted 1.0 so behaviour is initially almost
    // unchanged. That is what makes structural mutation survivable instead of
    // instantly lethal.
    if (rng.nextFloat() < addNodeRate && m_connCount[slot] > 0 &&
        m_nodeCount[slot] < m_nodeCapacity &&
        static_cast<uint16_t>(m_connCount[slot] + 2) <= m_connCapacity) {
        const uint16_t ci = static_cast<uint16_t>(rng.range(0, m_connCount[slot]));
        ConnGene& c = b.conns[ci];
        if (c.flags & ConnFlag_Enabled) {
            NodeGene ng;
            ng.id = m_nextInnovation++;
            ng.kind = static_cast<uint8_t>(NodeKind::Hidden);
            ng.activation = static_cast<uint8_t>(rng.range(0, static_cast<int>(Activation::Count)));
            ng.biasA = rng.gaussian(0.0f, 0.2f);
            ng.biasB = rng.gaussian(0.0f, 0.2f);
            const uint32_t newId = ng.id;
            const uint32_t from = c.from, to = c.to;
            const float w = 0.5f * (c.weightA + c.weightB);
            c.flags = static_cast<uint8_t>(c.flags & ~ConnFlag_Enabled);
            b.nodes[m_nodeCount[slot]++] = ng;
            sortNodes(slot);
            addConnection(slot, from, newId, 1.0f, rng);
            addConnection(slot, newId, to, w, rng);
        }
    }
}

// ---------------------------------------------------------------------------
// Development: express alleles, order the nodes, build the CSR index
// ---------------------------------------------------------------------------

void Brains::develop(size_t slot, const Phenotype& phenotype) {
    (void)phenotype;
    BrainView b = brain(slot);
    BrainRuntime& rt = m_runtime[slot];
    const uint16_t n = m_nodeCount[slot];
    const uint16_t nc = m_connCount[slot];

    for (uint16_t i = 0; i < n; ++i)
        rt.bias[i] = expressAllele(b.nodes[i].biasA, b.nodes[i].biasB, Dominance::Additive);
    for (uint16_t i = 0; i < nc; ++i)
        rt.weight[i] = expressAllele(b.conns[i].weightA, b.conns[i].weightB,
                                     static_cast<Dominance>(b.conns[i].dominance));

    // Resolve every connection's endpoints to node INDICES once, here, so the
    // per-tick forward pass never has to look an id up again.
    for (uint16_t c = 0; c < nc; ++c) {
        const int f = nodeIndexOf(b.nodes, n, b.conns[c].from);
        const int t = nodeIndexOf(b.nodes, n, b.conns[c].to);
        rt.connSrc[c] = static_cast<uint16_t>(f >= 0 ? f : 0);
        rt.connDst[c] = static_cast<uint16_t>(t >= 0 ? t : 0);
        // An endpoint that no longer exists (its node was lost in crossover)
        // disables the connection rather than silently reading node 0.
        if (f < 0 || t < 0) b.conns[c].flags = static_cast<uint8_t>(b.conns[c].flags & ~ConnFlag_Enabled);
    }

    // Longest-path depth from the inputs, relaxed iteratively. Cycles stop
    // contributing once the pass cap is hit, and those are precisely the edges
    // that should be treated as recurrent.
    std::vector<uint16_t>& depth = m_depth;
    depth.assign(n, 0);
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (uint16_t c = 0; c < nc; ++c) {
            if ((b.conns[c].flags & ConnFlag_Enabled) == 0) continue;
            const uint16_t want = static_cast<uint16_t>(depth[rt.connSrc[c]] + 1);
            if (want > depth[rt.connDst[c]] && want < 250) {
                depth[rt.connDst[c]] = want;
                changed = true;
            }
        }
        if (!changed) break;
    }
    // Outputs always evaluate last regardless of what the graph says, so a
    // motor command never lags a sensor reading by a whole tick.
    for (uint16_t i = 0; i < n; ++i)
        if (static_cast<NodeKind>(b.nodes[i].kind) == NodeKind::Output) depth[i] = 255;

    for (uint16_t c = 0; c < nc; ++c) {
        const bool recurrent = depth[rt.connSrc[c]] >= depth[rt.connDst[c]];
        if (recurrent) b.conns[c].flags |= ConnFlag_Recurrent;
        else b.conns[c].flags = static_cast<uint8_t>(b.conns[c].flags & ~ConnFlag_Recurrent);
    }

    // Evaluation order: ascending depth, ties broken by index, so the order is
    // a pure function of the topology and never of memory layout or timing.
    rt.orderCount = 0;
    for (uint16_t i = 0; i < n; ++i) rt.order[rt.orderCount++] = i;
    std::sort(rt.order, rt.order + rt.orderCount, [&depth](uint16_t a, uint16_t c) {
        if (depth[a] != depth[c]) return depth[a] < depth[c];
        return a < c;
    });

    // CSR adjacency by destination node: counting sort, same pattern as the
    // spatial hash. Turns the forward pass from O(nodes x conns) into O(conns).
    for (uint16_t i = 0; i <= n; ++i) rt.inStart[i] = 0;
    for (uint16_t c = 0; c < nc; ++c) {
        if ((b.conns[c].flags & ConnFlag_Enabled) == 0) continue;
        ++rt.inStart[rt.connDst[c] + 1];
    }
    for (uint16_t i = 0; i < n; ++i) rt.inStart[i + 1] = static_cast<uint16_t>(rt.inStart[i + 1] + rt.inStart[i]);
    {
        // Scratch sized from the capacity, not a fixed stack array: a
        // configurable node capacity must not silently drop connections.
        m_cursor.resize(static_cast<size_t>(m_nodeCapacity) + 1);
        for (uint16_t i = 0; i <= n; ++i) m_cursor[i] = rt.inStart[i];
        for (uint16_t c = 0; c < nc; ++c) {
            if ((b.conns[c].flags & ConnFlag_Enabled) == 0) continue;
            rt.inList[m_cursor[rt.connDst[c]]++] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

void Brains::evaluate(size_t slot, const float* inputs, float* outputs) {
    BrainView b = brain(slot);
    BrainRuntime& rt = m_runtime[slot];
    const uint16_t n = m_nodeCount[slot];

    // This step's values become next step's recurrent source.
    for (uint16_t i = 0; i < n; ++i) rt.previous[i] = rt.activation[i];

    // Inputs are ids 0..kBrainInputCount-1 and the node array is sorted by id,
    // so they are exactly the first kBrainInputCount entries.
    const uint16_t inputs_n = static_cast<uint16_t>(std::min<int>(kBrainInputCount, n));
    for (uint16_t i = 0; i < inputs_n; ++i) rt.activation[i] = inputs[i];

    for (uint16_t k = 0; k < rt.orderCount; ++k) {
        const uint16_t i = rt.order[k];
        if (static_cast<NodeKind>(b.nodes[i].kind) == NodeKind::Input) continue;

        float sum = rt.bias[i];
        const uint16_t begin = rt.inStart[i], end = rt.inStart[i + 1];
        for (uint16_t e = begin; e < end; ++e) {
            const uint16_t c = rt.inList[e];
            const uint16_t src = rt.connSrc[c];
            // Feed-forward edges read this step (the source is at a strictly
            // lower depth, so it is already computed); recurrent edges read the
            // previous step, which is what gives the network memory.
            const float v = (b.conns[c].flags & ConnFlag_Recurrent) ? rt.previous[src]
                                                                    : rt.activation[src];
            sum += rt.weight[c] * v;
        }
        rt.activation[i] = activate(static_cast<Activation>(b.nodes[i].activation), sum);
    }

    // Outputs are the block of ids immediately after the inputs.
    for (int o = 0; o < kBrainOutputCount; ++o) {
        const uint16_t idx = static_cast<uint16_t>(kBrainInputCount + o);
        outputs[o] = (idx < n) ? rt.activation[idx] : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Reward-modulated plasticity
// ---------------------------------------------------------------------------

void Brains::applyReward(size_t slot, float reward, const Phenotype& phenotype) {
    BrainView b = brain(slot);
    BrainRuntime& rt = m_runtime[slot];
    const uint16_t nc = m_connCount[slot];
    if (nc == 0) return;

    const float plasticity = phenotype.get(Trait::PlasticityRate);
    const float learningRate = phenotype.get(Trait::LearningRate);
    const float traceDecay = cfg().getF("brain.trace_decay", 0.92f);
    const float baselineRate = cfg().getF("brain.value_baseline_rate", 0.02f);
    const float weightDecay = cfg().getF("brain.weight_decay", 0.00002f);

    // The dopamine analogue: reward MINUS what was expected. A fully predicted
    // reward teaches nothing, which is what stops an agent endlessly
    // reinforcing a behaviour it has already mastered.
    const float rpe = reward - rt.valueBaseline;
    rt.valueBaseline += baselineRate * rpe;
    rt.lastReward = reward;
    rt.lastRpe = rpe;

    const uint8_t want = ConnFlag_Enabled | ConnFlag_Plastic;
    for (uint16_t c = 0; c < nc; ++c) {
        if ((b.conns[c].flags & want) != want) continue;

        // Hebbian eligibility: the trace records that this connection was
        // recently active, so a reward arriving several ticks later can still
        // find the connections that helped produce it. This is what lets an
        // entire behaviour chain -- approach, display, provisioning -- be
        // reinforced by one terminal reward rather than only the last step.
        const float pre = rt.previous[rt.connSrc[c]];
        const float post = rt.activation[rt.connDst[c]];
        rt.eligibility[c] = traceDecay * rt.eligibility[c] +
                            (1.0f - traceDecay) * pre * post * b.conns[c].plasticity * plasticity;

        float w = rt.weight[c] + learningRate * rpe * rt.eligibility[c];
        w -= w * weightDecay;
        rt.weight[c] = std::min(8.0f, std::max(-8.0f, w));
    }
}

uint16_t Brains::hiddenNodeCount(size_t slot) const {
    ConstBrainView b = brain(slot);
    uint16_t h = 0;
    for (uint16_t i = 0; i < b.nodeCount; ++i)
        if (static_cast<NodeKind>(b.nodes[i].kind) == NodeKind::Hidden) ++h;
    return h;
}

uint16_t Brains::enabledConnCount(size_t slot) const {
    ConstBrainView b = brain(slot);
    uint16_t c = 0;
    for (uint16_t i = 0; i < b.connCount; ++i)
        if (b.conns[i].flags & ConnFlag_Enabled) ++c;
    return c;
}

// ---------------------------------------------------------------------------

void Brains::serializeSlot(BinaryWriter& w, size_t slot) const {
    const uint16_t nn = m_nodeCount[slot], nc = m_connCount[slot];
    w.pod(nn);
    w.pod(nc);
    const NodeGene* nodes = &m_nodes[slot * m_nodeCapacity];
    for (uint16_t i = 0; i < nn; ++i) {
        w.pod(nodes[i].id); w.pod(nodes[i].kind); w.pod(nodes[i].activation);
        w.pod(nodes[i].biasA); w.pod(nodes[i].biasB);
    }
    const ConnGene* conns = &m_conns[slot * m_connCapacity];
    for (uint16_t i = 0; i < nc; ++i) {
        w.pod(conns[i].from); w.pod(conns[i].to); w.pod(conns[i].innovation);
        w.pod(conns[i].flags); w.pod(conns[i].dominance);
        w.pod(conns[i].weightA); w.pod(conns[i].weightB); w.pod(conns[i].plasticity);
    }
    // Learned weights are NOT recoverable from the genome -- they are what this
    // individual learned in its own life -- so they are saved explicitly.
    const float* weight = &m_weight[slot * m_connCapacity];
    const float* elig = &m_eligibility[slot * m_connCapacity];
    for (uint16_t i = 0; i < nc; ++i) { w.pod(weight[i]); w.pod(elig[i]); }
    w.pod(m_runtime[slot].valueBaseline);
}

void Brains::deserializeSlot(BinaryReader& r, size_t slot) {
    uint16_t nn = 0, nc = 0;
    r.pod(nn);
    r.pod(nc);
    if (nn > m_nodeCapacity) nn = m_nodeCapacity;
    if (nc > m_connCapacity) nc = m_connCapacity;

    NodeGene* nodes = &m_nodes[slot * m_nodeCapacity];
    for (uint16_t i = 0; i < nn; ++i) {
        r.pod(nodes[i].id); r.pod(nodes[i].kind); r.pod(nodes[i].activation);
        r.pod(nodes[i].biasA); r.pod(nodes[i].biasB);
        if (nodes[i].id >= m_nextInnovation) m_nextInnovation = nodes[i].id + 1;
    }
    ConnGene* conns = &m_conns[slot * m_connCapacity];
    for (uint16_t i = 0; i < nc; ++i) {
        r.pod(conns[i].from); r.pod(conns[i].to); r.pod(conns[i].innovation);
        r.pod(conns[i].flags); r.pod(conns[i].dominance);
        r.pod(conns[i].weightA); r.pod(conns[i].weightB); r.pod(conns[i].plasticity);
        if (conns[i].innovation >= m_nextInnovation) m_nextInnovation = conns[i].innovation + 1;
    }
    float* weight = &m_weight[slot * m_connCapacity];
    float* elig = &m_eligibility[slot * m_connCapacity];
    for (uint16_t i = 0; i < nc; ++i) { r.pod(weight[i]); r.pod(elig[i]); }
    r.pod(m_runtime[slot].valueBaseline);

    m_nodeCount[slot] = nn;
    m_connCount[slot] = nc;
}

}  // namespace gen
