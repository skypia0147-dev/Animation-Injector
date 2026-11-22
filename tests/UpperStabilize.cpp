// The upper layer's stabilizer (core/UpperStabilize.h): the legs' tilt and sway never reach the spine, their heading
// (about the up axis) turns it; the clip's own parent lean is dropped, its stance turn kept; the blend weight goes from
// the output's own rotation to it.
#include "core/UpperStabilize.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace AnimationInjector::UpperStabilize;
namespace {
unsigned checks = 0;
void Check(bool b, const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
Quat Axis(float x, float y, float z, float degrees) {
    const float h = degrees * 3.14159265f / 360.0f, n = std::sqrt(x * x + y * y + z * z);
    return {x / n * std::sin(h), y / n * std::sin(h), z / n * std::sin(h), std::cos(h)};
}
// The same rotation (q and -q are one).
bool Same(const Quat& a, const Quat& b, float eps = 1e-4f) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return std::fabs(std::fabs(dot) - 1.0f) < eps;
}
float Angle(const Quat& a, const Quat& b) {
    const float dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2.0f * std::acos(std::fmin(1.0f, dot)) * 180.0f / 3.14159265f;
}
}  // namespace

int main() {
    try {
        const Quat id{0, 0, 0, 1};
        // A chain multiplies parent first.
        const Quat yaw = Axis(0, 0, 1, 30), pitch = Axis(1, 0, 0, 20);
        const std::vector<Quat> chain{yaw, pitch};
        Check(Same(ChainRotation(chain),
                  Quat{yaw[3] * pitch[0] + yaw[0] * pitch[3] + yaw[1] * pitch[2] - yaw[2] * pitch[1],
                      yaw[3] * pitch[1] - yaw[0] * pitch[2] + yaw[1] * pitch[3] + yaw[2] * pitch[0],
                      yaw[3] * pitch[2] + yaw[0] * pitch[1] - yaw[1] * pitch[0] + yaw[2] * pitch[3],
                      yaw[3] * pitch[3] - yaw[0] * pitch[0] - yaw[1] * pitch[1] - yaw[2] * pitch[2]}),
            "chain = parent * child");
        Check(Same(ChainRotation(std::vector<Quat>{}), id), "an empty chain is no rotation");

        // The point of it. `rest` stands for the parent's reference rotation (not identity in the game's skeleton).
        const Quat rest = ChainRotation(std::vector<Quat>{Axis(0, 0, 1, 90), Axis(1, 0, 0, 90)});
        const Quat spineLocal = ChainRotation(std::vector<Quat>{Axis(0, 0, 1, 5), Axis(1, 0, 0, -8)});
        const auto model = [](const Quat& a, const Quat& b) { return ChainRotation(std::vector<Quat>{a, b}); };
        // The spine's model rotation the layer gives, for the legs' parent and the clip's parent and spine.
        const auto result = [&](const Quat& parent, const Quat& clipParent, const Quat& clipSpine) {
            return model(parent, SpineLocal(parent, rest, clipParent, clipSpine, Axis(0, 0, 1, 40), 1.0f));
        };
        const Quat upright = model(rest, spineLocal);  // the clip's spine on its parent at rest
        // Tilts: rotations about a level axis (pure swings), applied in model space over the rest rotation.
        for (const Quat tilt : {id, Axis(1, 0, 0, 13), Axis(0, 1, 0, -11), Axis(1, 1, 0, 15)}) {
            Check(Same(result(model(tilt, rest), rest, upright), upright),
                "the legs' parent leans: the spine keeps the clip's rotation");
            Check(Same(result(rest, model(tilt, rest), model(model(tilt, rest), spineLocal)), upright),
                "the clip's parent leans (a crouch): the spine stands as on an upright parent");
        }
        for (const float heading : {26.0f, 90.0f, -90.0f, 180.0f}) {
            const Quat turned = Axis(0, 0, 1, heading);
            Check(Same(result(model(turned, rest), rest, upright), model(turned, upright)),
                "the legs' parent turns: the spine turns by the same heading");
            Check(Same(result(model(model(turned, Axis(1, -1, 0, 16)), rest), rest, upright), model(turned, upright)),
                "turned and leaning legs: only the heading reaches the spine");
            const Quat clipStance = model(turned, rest);
            Check(Same(result(rest, clipStance, model(clipStance, spineLocal)), model(clipStance, spineLocal)),
                "the clip's own stance turn stays");
        }
        Check(Same(TwistZ(ChainRotation(std::vector<Quat>{Axis(0, 0, 1, 33), Axis(1, 0, 0, 20)})), Axis(0, 0, 1, 33)),
            "twist of yaw * pitch = the yaw");
        Check(Same(TwistZ(Axis(1, 0, 0, 180)), id), "a half turn about a level axis has no twist");
        const Quat clipSpine = upright;
        // w 0 leaves the output's own rotation; between, the arc is shortest and monotonic.
        const Quat outputLocal = Axis(0, 0, 1, 40), pelvis = Axis(0, 0, 1, 26);
        Check(Same(SpineLocal(pelvis, id, id, clipSpine, outputLocal, 0.0f), outputLocal), "w 0: the output's own");
        const Quat target = SpineLocal(pelvis, id, id, clipSpine, outputLocal, 1.0f);
        const float whole = Angle(outputLocal, target);
        float last = -1.0f;
        for (float w : {0.25f, 0.5f, 0.75f}) {
            const float moved = Angle(outputLocal, SpineLocal(pelvis, id, id, clipSpine, outputLocal, w));
            Check(std::fabs(moved - whole * w) < 0.05f, "slerp moves w of the way");
            Check(moved > last, "and farther with w");
            last = moved;
        }
        // q and -q: the same rotation, the short way.
        const Quat negated{-outputLocal[0], -outputLocal[1], -outputLocal[2], -outputLocal[3]};
        Check(Angle(negated, SpineLocal(pelvis, id, id, clipSpine, negated, 0.5f)) < whole * 0.5f + 0.05f,
            "a negated quaternion blends the short way");
        // Not a unit quaternion in: still a unit quaternion out.
        const Quat out = SpineLocal({0, 0, 0.5f, 1.2f}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 2}, {0, 0, 0, 3}, 0.5f);
        Check(std::fabs(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3] - 1.0f) < 1e-4f,
            "normalized");
        // facing="camera" (AimTurn): a spine already at the clip's own heading is turned by the camera's alone.
        {
            const Quat rest = Axis(0, 1, 0, 90), clip = Multiply(Axis(0, 0, 1, -40), rest);
            Check(Same(AimTurn(clip, clip, rest, 0.0f, 0.0f), id), "aim: no camera turn, no turn");
            Check(Same(AimTurn(clip, clip, rest, 0.5f, 0.0f), Axis(0, 0, 1, 0.5f * 180.0f / 3.14159265f)),
                "aim: the camera's yaw about the up axis");
            // The legs turned the spine 70 degrees away (a side step): the aim takes that back.
            const Quat turned = Multiply(Axis(0, 0, 1, 70), clip);
            Check(Same(Multiply(AimTurn(clip, turned, rest, 0.0f, 0.0f), turned), clip), "aim: the legs' turn undone");
            // Pitch about the aim's side axis: looking straight ahead (+Y) and up raises +Y toward +Z.
            Check(Same(AimTurn(clip, clip, rest, 0.0f, 0.6f), Axis(1, 0, 0, 0.6f * 180.0f / 3.14159265f)),
                "aim: pitch about +X when facing +Y");
            // Turned left 90 degrees, the side axis is +Y: the pitch is about it.
            Check(Same(AimTurn(clip, clip, rest, 1.5707963f, 0.6f),
                      Multiply(Axis(0, 1, 0, 0.6f * 180.0f / 3.14159265f), Axis(0, 0, 1, 90))),
                "aim: pitch about the turned side axis");
            // Three equal shares through a chain give the whole turn at its top bone.
            const Quat parent = Axis(1, 1, 0, 25), l0 = Axis(0, 1, 0, 10), l1 = Axis(1, 0, 0, 7), l2 = Axis(0, 0, 1, 5);
            const Quat turn = AimTurn(clip, turned, rest, 0.4f, -0.3f), share = Slerp(id, turn, 1.0f / 3.0f);
            const Quat m0 = Multiply(parent, l0), m1 = Multiply(m0, l1), m2 = Multiply(m1, l2);
            const Quat n0 = TurnLocal(parent, share, l0), n1 = TurnLocal(m0, share, l1), n2 = TurnLocal(m1, share, l2);
            const Quat top = Multiply(Multiply(Multiply(parent, n0), n1), n2);
            Check(Same(top, Multiply(turn, m2)), "aim: shared over three bones");
            Check(Angle(Multiply(parent, n0), Multiply(share, m0)) < 0.01f, "aim: the first bone takes a third");
        }
        // The waist (SpreadWaist): the turn from the pelvis spread over the three spine joints, the chest kept.
        {
            const std::array<Quat, 4> rests{id, id, id, id};
            const Quat com = Axis(0, 0, 1, 10);
            const std::array<Quat, 3> locals{Axis(0, 0, 1, -60), Axis(1, 0, 0, 5), Axis(0, 0, 1, 0)};
            const auto out = SpreadWaist(com, id, locals, rests, 1.0f);
            const Quat before = Multiply(Multiply(Multiply(com, locals[0]), locals[1]), locals[2]);
            const Quat after = Multiply(Multiply(Multiply(com, out[0]), out[1]), out[2]);
            Check(Same(before, after, 1e-3f), "waist: the chest keeps its pose");
            const auto turn = [](const Quat& q) { return 2.0f * std::atan2(TwistZ(q)[2], TwistZ(q)[3]) * 57.29578f; };
            const Quat m0 = Multiply(com, out[0]), m1 = Multiply(m0, out[1]), m2 = Multiply(m1, out[2]);
            const float j0 = turn(m0) - turn(com), j1 = turn(m1) - turn(m0), j2 = turn(m2) - turn(m1);
            Check(std::fabs(j0 + 20.0f) < 0.5f && std::fabs(j1 + 20.0f) < 0.5f && std::fabs(j2 + 20.0f) < 0.5f,
                "waist: each joint takes a third of -60 degrees");
            const auto none = SpreadWaist(com, id, locals, rests, 0.0f);
            Check(Same(none[0], locals[0]) && Same(none[1], locals[1]) && Same(none[2], locals[2]), "waist: amount 0");
        }
        std::cout << "UpperStabilize: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UpperStabilize FAILED after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
