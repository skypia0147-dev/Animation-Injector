// The generated vanilla ladder table (core/VanillaLadders.inl, tools/gen_vanilla_ladders.py): lookups and the values
// the in-game read of 11-14 confirmed.
#include "core/VanillaLadders.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace AnimationInjector;
namespace {
unsigned checks = 0;
void Check(bool b, const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
}  // namespace

int main() {
    try {
        const auto* f = VanillaLadders::Find("Melee_Locomotion_Behavior", "1HM_ForwardBlend");
        Check(f && f->slots.size() == 4, "1HM forward has four slots");
        Check(std::strcmp(f->slots[1].clip, "1HM_WalkForward") == 0 && Near(f->slots[1].weight, 58.4995f) &&
                  Near(f->slots[1].playback, 1.0f) && f->slots[1].mode == 1,
            "the walk slot");
        Check(Near(f->slots[0].playback, 0.1f) && Near(f->slots[3].playback, 10.0f), "slow and fast playback");
        Check(f->slots[1].triggers.size() == 2 && std::strcmp(f->slots[1].triggers[0].event, "FootLeft") == 0 &&
                  Near(f->slots[1].triggers[0].time, 0.0333f),
            "the walk slot's footstep triggers");
        Check(VanillaLadders::Find("melee_locomotion_behavior", "1hm_forwardblend") == f, "names ignore case");
        {
            const auto* block = VanillaLadders::Find("BlockBehavior", "2HM_LeftBlend");
            Check(block && block != VanillaLadders::Find("Melee_Locomotion_Behavior", "2HM_LeftBlend") &&
                      block->slots.size() == 5 && std::strcmp(block->slots[2].clip, "2HM_RunStrafeLeft") == 0 &&
                      Near(block->slots[2].weight, 154.496f) && std::strcmp(block->graph, "BlockBehavior") == 0,
                "block movement has records of its own");
        }
        Check(!VanillaLadders::Find("Melee_Locomotion_Behavior", "1HM_ForwardBlendX"), "unknown blender");
        {
            const auto* hm = VanillaLadders::Find("Melee_Locomotion_Behavior", "2HM_LeftBlend");
            const auto* hw = VanillaLadders::Find("Melee_Locomotion_Behavior", "2HW_LeftBlend");
            Check(hm && hw && hw != hm && hw->slots.size() == hm->slots.size(), "2HW reads the 2HM record");
            Check(std::strcmp(hw->blender, "2HW_LeftBlend") == 0 && std::strncmp(hw->slots[1].clip, "2HW_", 4) == 0 &&
                      std::strcmp(hw->slots[1].clip + 4, hm->slots[1].clip + 4) == 0 &&
                      std::strcmp(hw->slots[1].animation, hm->slots[1].animation) == 0 &&
                      Near(hw->slots[1].weight, hm->slots[1].weight),
                "2HW clip names renamed, the rest as 2HM");
            Check(VanillaLadders::Find("melee_locomotion_behavior", "2hw_leftblend") == hw, "the same record again");
            Check(!VanillaLadders::Find("Melee_Locomotion_Behavior", "2HW_LeftBlendX"), "unknown 2HW blender");
            // H2H_<Direction>Blend (unarmed's own tree): the 1HM record, in the block graph too.
            const auto* one = VanillaLadders::Find("Melee_Locomotion_Behavior", "1HM_LeftBlend");
            const auto* h2h = VanillaLadders::Find("Melee_Locomotion_Behavior", "H2H_LeftBlend");
            Check(one && h2h && h2h != one && h2h->slots.size() == one->slots.size(), "H2H reads the 1HM record");
            Check(std::strcmp(h2h->blender, "H2H_LeftBlend") == 0 && std::strncmp(h2h->slots[1].clip, "H2H_", 4) == 0 &&
                      std::strcmp(h2h->slots[1].animation, one->slots[1].animation) == 0 &&
                      h2h->slots[1].weight == one->slots[1].weight,
                "H2H clip names renamed, the rest as 1HM");
            Check(VanillaLadders::Find("BlockBehavior", "H2H_ForwardBlend") != nullptr, "the block graph's H2H record");
        }
        const auto* left = VanillaLadders::Find("Melee_Locomotion_Behavior", "1HM_LeftBlend");
        Check(left && left->slots.size() == 5 && std::strcmp(left->slots[2].clip, "1HM_RunStrafeLeft") == 0,
            "the strafe slot of a side ladder");
        const auto* sneak = VanillaLadders::Find("1HM_Behavior", "Sneak_ForwardBlend");
        Check(sneak && std::strcmp(sneak->slots[0].clip, "1stP_Sneak_WalkForwardSlow") == 0,
            "the same blender name differs per graph");
        for (const char* graph : {"Melee_Locomotion_Behavior", "MT_RootBehavior", "MagicBehavior", "Master_Behavior"})
            for (const char* blender :
                {"1HM_BackwardBlend", "MT_BackwardBlend", "Magic_BackwardBlend", "Sneak_BackwardBlend"}) {
                const auto* l = VanillaLadders::Find(graph, blender);
                if (!l) continue;
                for (std::size_t i = 1; i < l->slots.size(); ++i)
                    Check(l->slots[i].weight > l->slots[i - 1].weight, "weights ascend");
            }
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
    std::cout << "vanilla ladder checks passed: " << checks << '\n';
    return 0;
}
