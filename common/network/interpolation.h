#pragma once

#include "protocol.h"
#include <algorithm>
#include <cmath>

namespace Coop {

// Presentation follows the latest validated target; it never predicts movement.
class PoseInterpolation final {
  public:
    static constexpr uint64_t DurationMs = 100;
    void reset() { target.reset(); progress = 1.f; }
    bool empty() const { return !target; }
    float alpha() const { return progress; }

    void push(const Message& frame, const std::array<ActorPose,2>& displayed, uint64_t now) {
      const auto& next = std::get<Poses>(frame.payload);
      bool changed = !target;
      bool discontinuity = false;
      if(target) {
        const auto& previous = std::get<Poses>(target->payload);
        discontinuity = target->generation!=frame.generation || previous.world!=next.world;
        for(size_t i=0; i<2; ++i) {
          changed |= !sameTransform(previous.actors[i],next.actors[i]);
          discontinuity |= previous.actors[i].object!=next.actors[i].object;
          }
        }
      for(size_t i=0; i<2; ++i) {
        float distance2 = 0;
        for(size_t axis=0; axis<3; ++axis) {
          const float d = next.actors[i].position[axis]-displayed[i].position[axis];
          distance2 += d*d;
          }
        discontinuity |= distance2>1000.f*1000.f;
        }
      if(changed || discontinuity) {
        origin = displayed;
        started = now;
        snap = discontinuity;
        }
      target = frame;
      }

    Message sample(uint64_t now) {
      if(!target) throw std::logic_error("No pose interpolation target");
      progress = snap ? 1.f : std::min(1.f,float(now>=started ? now-started : 0)/float(DurationMs));
      Message result = *target;
      if(progress==1.f) return result; // Preserve exact endpoint floats.
      auto& poses = std::get<Poses>(result.payload);
      for(size_t i=0; i<2; ++i) {
        auto& actor = poses.actors[i];
        const auto& from = origin[i];
        for(size_t axis=0; axis<3; ++axis)
          actor.position[axis] = mix(from.position[axis],actor.position[axis],progress);
        const float turn = std::remainder(actor.yaw-from.yaw,360.f);
        actor.yaw = from.yaw+turn*progress;
        actor.pitch = mix(from.pitch,actor.pitch,progress);
        actor.roll = mix(from.roll,actor.roll,progress);
        }
      return result;
      }

  private:
    static float mix(float a, float b, float alpha) { return a+(b-a)*alpha; }
    static bool sameTransform(const ActorPose& a, const ActorPose& b) {
      return a.position==b.position && a.yaw==b.yaw && a.pitch==b.pitch && a.roll==b.roll;
      }
    std::optional<Message> target;
    std::array<ActorPose,2> origin;
    uint64_t started = 0;
    float progress = 1.f;
    bool snap = false;
  };

}
