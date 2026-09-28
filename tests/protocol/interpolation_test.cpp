#include "network/interpolation.h"
#include <iostream>
#include <stdexcept>

using namespace Coop;
#define REQUIRE(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)

Message frame(float x, float yaw=0) {
  Poses poses;
  poses.world = 1;
  for(size_t i=0; i<2; ++i) {
    poses.actors[i].player = poses.actors[i].object = i+1;
    poses.actors[i].position[0] = x+float(i)*100;
    poses.actors[i].yaw = yaw;
    }
  return {1,2,0,2,poses};
  }

int main(int argc, char** argv) {
  try {
    REQUIRE(argc==2);
    PoseInterpolation interpolation;
    const auto origin = std::get<Poses>(frame(0,179).payload).actors;
    interpolation.push(frame(100,-179),origin,1000);
    const std::string test = argv[1];
    if(test=="motion") {
      auto half = std::get<Poses>(interpolation.sample(1050).payload);
      REQUIRE(half.actors[0].position[0]==50 && half.actors[1].position[0]==150);
      REQUIRE(half.actors[0].yaw==180 && interpolation.alpha()==0.5f);
      // New targets start from the currently displayed point, with no jump.
      interpolation.push(frame(200,-178),half.actors,1050);
      REQUIRE(std::get<Poses>(interpolation.sample(1050).payload).actors[0].position[0]==50);
      REQUIRE(std::get<Poses>(interpolation.sample(1100).payload).actors[0].position[0]==125);
      // Stationary updates must not restart the interpolation indefinitely.
      auto next = frame(200,-178); next.revision = 7;
      interpolation.push(next,std::get<Poses>(interpolation.sample(1125).payload).actors,1125);
      REQUIRE(interpolation.sample(1150)==next && interpolation.alpha()==1);
      REQUIRE(interpolation.sample(999999)==next); // Never extrapolate.
      }
    else if(test=="discontinuities") {
      REQUIRE(std::get<Poses>(interpolation.sample(500).payload).actors==origin); // Clock went backward.
      auto teleport = frame(5000);
      interpolation.push(teleport,origin,2000);
      REQUIRE(interpolation.sample(2000)==teleport);
      auto generation = frame(10); generation.generation = 2;
      interpolation.push(generation,origin,3000);
      REQUIRE(interpolation.sample(3000)==generation);
      auto world = frame(20); world.generation = 2;
      std::get<Poses>(world.payload).world = 2;
      interpolation.push(world,origin,4000);
      REQUIRE(interpolation.sample(4000)==world);
      interpolation.reset();
      REQUIRE(interpolation.empty());
      }
    else throw std::runtime_error("Unknown test");
    std::cout << "PASS: " << test << '\n';
    return 0;
    }
  catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  }
