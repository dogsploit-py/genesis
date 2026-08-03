// sim/brain.h — genome-encoded recurrent neural networks with lifetime learning.
//
// Two timescales, as specified:
//
//   1. EVOLUTIONARY. Topology and weights live in the genome. Connections are
//      matched between parents by innovation number (NEAT-style), so two brains
//      with different structures can still be crossed sensibly. Topology
//      mutations add nodes, add connections and disable connections, so brain
//      architecture evolves rather than only its weights.
//
//   2. LIFETIME. Connections flagged plastic carry an eligibility trace updated
//      Hebbianly. A dopamine-analog reward-prediction error then scales those
//      traces into weight changes. An agent therefore learns within its life,
//      and because what it learns changes which genotypes survive, the Baldwin
//      effect is observable.
//
// Deliberate design note: connection WEIGHTS are diploid, like the rest of the
// genome -- each connection carries two alleles combined under a dominance
// model. Connection PRESENCE is inherited by innovation-number alignment
// instead, because there is no meaningful dominance relation between "this
// connection exists" and "it does not". Matching genes combine both alleles;
// disjoint and excess genes are inherited from whichever parent has them.
#pragma once

#include <cstdint>
#include <vector>

#include "core/rng.h"
#include "sim/genetics.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

// ---------------------------------------------------------------------------
// Sensory and motor layout
// ---------------------------------------------------------------------------

enum BrainInput : int {
    // Vision: six sectors around the heading, three channels each. This is the
    // rasterised local view -- what is edible, what is alive, where water is.
    In_VisionPlant0 = 0, In_VisionPlant1, In_VisionPlant2,
    In_VisionPlant3, In_VisionPlant4, In_VisionPlant5,
    In_VisionAgent0, In_VisionAgent1, In_VisionAgent2,
    In_VisionAgent3, In_VisionAgent4, In_VisionAgent5,
    In_VisionWater0, In_VisionWater1, In_VisionWater2,
    In_VisionWater3, In_VisionWater4, In_VisionWater5,

    // Interoception.
    In_Hunger, In_Thirst, In_Energy, In_Health, In_Pain, In_Stress,
    In_BodyTemperature, In_Age, In_ReproductiveReadiness,

    // Ambient environment.
    In_Light, In_AmbientTemperature, In_Terrain, In_SoilMoisture,

    // The nearest other agent.
    In_NearestBearing, In_NearestDistance, In_NearestSexExpression,
    In_NearestAttraction, In_NearestRelatedness, In_NearestIsBonded,

    // Evolved communication channels, averaged over audible neighbours.
    In_Signal0, In_Signal1, In_Signal2, In_Signal3,

    // Memory recall.
    In_MemoryFoodDirection, In_MemoryWaterDirection,

    // Recurrent working state, written back by the network itself.
    In_Recurrent0, In_Recurrent1, In_Recurrent2, In_Recurrent3,

    // Constant 1.0, so the network always has a bias source.
    In_Bias,

    kBrainInputCount
};

enum BrainOutput : int {
    Out_Turn = 0,
    Out_Accelerate,
    Out_Eat,
    Out_Drink,
    Out_PickUp,
    Out_Drop,
    Out_Use,
    Out_Attack,
    Out_Flee,
    Out_Groom,
    Out_CourtshipDisplay,
    Out_AcceptMating,
    Out_Vocalise0, Out_Vocalise1, Out_Vocalise2, Out_Vocalise3,
    Out_Teach,
    Out_Share,
    Out_Build,
    Out_Rest,
    kBrainOutputCount
};

const char* brainInputName(int i);
const char* brainOutputName(int i);

static_assert(kBrainInputCount == 48, "spec calls for ~48 sensory inputs");
static_assert(kBrainOutputCount == 20, "spec calls for ~20 motor outputs");

// ---------------------------------------------------------------------------
// Drives
// ---------------------------------------------------------------------------

enum class Drive : uint8_t {
    Hunger = 0, Thirst, Thermoregulation, Safety, Rest, Social, Curiosity, Reproduction,
    Count
};
constexpr int kDriveCount = static_cast<int>(Drive::Count);

const char* driveName(Drive d);
// Which heritable reward-weight trait governs each drive, so that motivation
// itself is under selection.
Trait driveRewardTrait(Drive d);

struct DriveState {
    float level[kDriveCount] = {0.0f};      // current deficit, 0 = satisfied
    float setpoint[kDriveCount] = {0.0f};
    float lastSatisfaction[kDriveCount] = {0.0f};

    float& operator[](Drive d) { return level[static_cast<int>(d)]; }
    float  operator[](Drive d) const { return level[static_cast<int>(d)]; }
};

// ---------------------------------------------------------------------------
// Brain genome
// ---------------------------------------------------------------------------

enum class Activation : uint8_t { Tanh = 0, Sigmoid, Relu, Gaussian, Sine, Linear, Count };
const char* activationName(Activation a);

enum class NodeKind : uint8_t { Input = 0, Output, Hidden };

// Ids are 32-bit and globally unique for the lifetime of the run. 16 bits would
// wrap after 65k structural mutations, which a long run passes in minutes, and
// a wrapped id would silently align two unrelated connections during crossover.
//
// INVARIANT: a brain's node array is always sorted by id. Input ids are
// 0..kBrainInputCount-1 and output ids follow, so inputs and outputs sit at
// fixed indices and only hidden nodes need looking up.
struct NodeGene {
    uint32_t id = 0;
    uint8_t  kind = 0;        // NodeKind
    uint8_t  activation = 0;  // Activation
    uint16_t pad = 0;
    float    biasA = 0.0f;
    float    biasB = 0.0f;
};
static_assert(sizeof(NodeGene) == 16, "NodeGene must stay packed");

enum ConnFlags : uint8_t {
    ConnFlag_None     = 0,
    ConnFlag_Enabled  = 1 << 0,
    ConnFlag_Plastic  = 1 << 1,   // subject to reward-modulated Hebbian learning
    ConnFlag_Recurrent = 1 << 2,  // computed at phenotype build, not inherited
};

struct ConnGene {
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t innovation = 0;   // homology identity for crossover
    uint8_t  flags = 0;
    uint8_t  dominance = 0;    // how the two weight alleles combine
    uint16_t pad = 0;
    float    weightA = 0.0f;
    float    weightB = 0.0f;
    float    plasticity = 0.0f;
};
static_assert(sizeof(ConnGene) == 28, "ConnGene must stay packed");

struct BrainView {
    NodeGene* nodes = nullptr;
    ConnGene* conns = nullptr;
    uint16_t  nodeCount = 0;
    uint16_t  connCount = 0;
    uint16_t  nodeCapacity = 0;
    uint16_t  connCapacity = 0;
};

struct ConstBrainView {
    const NodeGene* nodes = nullptr;
    const ConnGene* conns = nullptr;
    uint16_t nodeCount = 0;
    uint16_t connCount = 0;

    ConstBrainView() = default;
    ConstBrainView(const BrainView& v)
        : nodes(v.nodes), conns(v.conns), nodeCount(v.nodeCount), connCount(v.connCount) {}
};

// Runtime state: expressed weights, activations, eligibility traces, the
// evaluation order, and a CSR adjacency index. Rebuilt only when the topology
// changes, never per tick.
//
// The CSR index is what makes evaluation affordable. Without it, finding each
// node's incoming edges means scanning every connection for every node --
// O(nodes x connections), which at 10k agents is hundreds of millions of
// operations per tick. With it, a forward pass is O(connections).
struct BrainRuntime {
    float*    activation = nullptr;     // nodeCapacity
    float*    previous = nullptr;       // nodeCapacity, for recurrent edges
    float*    weight = nullptr;         // connCapacity, expressed from alleles
    float*    eligibility = nullptr;    // connCapacity
    float*    bias = nullptr;           // nodeCapacity, expressed
    uint16_t* order = nullptr;          // nodeCapacity, evaluation order
    uint16_t* connSrc = nullptr;        // connCapacity, source node INDEX
    uint16_t* connDst = nullptr;        // connCapacity, destination node INDEX
    uint16_t* inStart = nullptr;        // nodeCapacity+1, CSR offsets by node
    uint16_t* inList = nullptr;         // connCapacity, connection indices
    uint16_t  orderCount = 0;
    float     valueBaseline = 0.0f;     // running reward prediction
    float     lastReward = 0.0f;
    float     lastRpe = 0.0f;
};

// ---------------------------------------------------------------------------

class Brains {
public:
    void configure(size_t maxAgents, uint16_t nodeCapacity, uint16_t connCapacity);

    uint16_t nodeCapacity() const { return m_nodeCapacity; }
    uint16_t connCapacity() const { return m_connCapacity; }
    uint16_t maxHiddenNodes() const {
        return static_cast<uint16_t>(m_nodeCapacity - kBrainInputCount - kBrainOutputCount);
    }

    BrainView      brain(size_t slot);
    ConstBrainView brain(size_t slot) const;
    BrainRuntime&  runtime(size_t slot) { return m_runtime[slot]; }
    const BrainRuntime& runtime(size_t slot) const { return m_runtime[slot]; }

    // A founder brain: inputs and outputs only, sparsely connected. Structure
    // has to be earned by mutation, so a run does not start with a solution.
    void makeFounder(size_t slot, Rng& rng);

    // NEAT-style crossover aligned on innovation numbers, then mutation.
    void inherit(size_t childSlot, size_t motherSlot, size_t fatherSlot,
                 Rng& rng, float mutationRateMultiplier);

    void copyBrain(size_t dstSlot, size_t srcSlot);
    void resetBrain(size_t slot, Rng& rng);

    // Expresses allele pairs into runtime weights and recomputes the evaluation
    // order. Call after birth, after a topology mutation, or after the UI edits
    // a weight.
    void develop(size_t slot, const Phenotype& phenotype);

    // One forward pass. Feed-forward edges read this step's values; recurrent
    // edges read the previous step's, so the network genuinely has memory
    // without costing a reaction-time delay through its whole depth.
    void evaluate(size_t slot, const float* inputs, float* outputs);

    // Reward-modulated plasticity. `reward` is the scalar the drive system
    // emitted this tick; the running baseline turns it into a prediction error,
    // and the eligibility traces decide which connections get credit.
    void applyReward(size_t slot, float reward, const Phenotype& phenotype);

    // Structural statistics for the charts.
    uint16_t hiddenNodeCount(size_t slot) const;
    uint16_t enabledConnCount(size_t slot) const;

    void serializeSlot(BinaryWriter& w, size_t slot) const;
    void deserializeSlot(BinaryReader& r, size_t slot);

    uint32_t nextInnovation() const { return m_nextInnovation; }

    // Index of a node id within a brain, via binary search on the sorted node
    // array. Returns -1 if absent.
    static int nodeIndexOf(const NodeGene* nodes, uint16_t count, uint32_t id);

private:
    // Writes a small set of INNATE REFLEXES into a founder's genome: drink when
    // thirsty, eat when hungry, turn toward what you can see, flee when hurt.
    //
    // This is not the simulation solving its own problem. Newborn mammals root
    // and suckle without learning, and a lineage with no innate reflexes at all
    // cannot survive long enough for learning or selection to get started -- a
    // purely random network dehydrates in about ten days, which is exactly what
    // happens with `brain.innate_reflexes` turned off. The weights go into the
    // GENOME, so evolution is free to strengthen, weaken or discard every one
    // of them, and usually does.
    void applyInnateReflexes(size_t slot, Rng& rng);
    void setConnection(size_t slot, uint32_t from, uint32_t to, float weight, Rng& rng);

    void addConnection(size_t slot, uint32_t from, uint32_t to, float weight, Rng& rng);
    void mutateTopology(size_t slot, Rng& rng, float rateMultiplier);
    void sortNodes(size_t slot);

    std::vector<NodeGene> m_nodes;
    std::vector<ConnGene> m_conns;
    std::vector<uint16_t> m_nodeCount;
    std::vector<uint16_t> m_connCount;

    std::vector<float>    m_activation, m_previous, m_weight, m_eligibility, m_bias;
    std::vector<uint16_t> m_order, m_connSrc, m_connDst, m_inStart, m_inList;
    std::vector<BrainRuntime> m_runtime;
    std::vector<uint16_t> m_depth;   // scratch for the ordering pass
    std::vector<uint16_t> m_cursor;  // scratch for the CSR counting sort

    uint16_t m_nodeCapacity = 128;
    uint16_t m_connCapacity = 224;
    uint32_t m_nextInnovation = 1;
    size_t   m_slotCount = 0;
};

}  // namespace gen
