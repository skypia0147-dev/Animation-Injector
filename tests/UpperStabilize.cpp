// The upper layer's stabilizer (core/UpperStabilize.h): the legs' tilt and sway never reach the spine, their heading
// (about the up axis) turns it; the clip's own parent lean is dropped, its stance turn kept; the blend weight goes from
// the output's own rotation to it.
#include "core/UpperStabilize.h"
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
        std::cout << "UpperStabilize: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UpperStabilize FAILED after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
