// Generate an original, asset-free Gothic II test world using bundled ZenKit.
#include <zenkit/Archive.hh>
#include <zenkit/Stream.hh>
#include <zenkit/World.hh>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

// ZenKit writes date/user metadata in outer and nested archives. Normalize at
// write time, so its size/offset bookkeeping sees the deterministic byte count.
// This wrapper applies only to our new fixture, never to installed game files.
class FixtureWriter final : public zenkit::Write {
  public:
    explicit FixtureWriter(std::unique_ptr<zenkit::Write> stream):stream(std::move(stream)) {}
    size_t write(const void* bytes,size_t size) noexcept override {
      const std::string_view value(static_cast<const char*>(bytes),size);
      if(next==1) { next=0; stream->write_string("1.1.2000 00:00:00"); return size; }
      if(next==2) { next=0; stream->write_string("OpenGothic"); return size; }
      if(value=="date ") next=1;
      if(value=="user ") next=2;
      return stream->write(bytes,size);
      }
    void seek(ssize_t offset,zenkit::Whence whence) noexcept override { next=0; stream->seek(offset,whence); }
    size_t tell() const noexcept override { return stream->tell(); }
  private:
    std::unique_ptr<zenkit::Write> stream;
    int next=0;
  };

int main(int argc, char** argv) {
  try {
    if(argc!=2) throw std::runtime_error("Usage: create-coop-arena OUTPUT.ZEN");
    if(std::filesystem::exists(argv[1])) throw std::runtime_error("Refusing to overwrite an existing arena");
    zenkit::World world;
    auto& mesh=world.world_mesh;
    mesh.name="COOP_TEST";
    mesh.bbox={{-2000,0,-2000},{2000,400,2000}};
    mesh.obb.center={0,200,0};
    mesh.obb.half_width={2000,200,2000};
    mesh.obb.axes[0]={1,0,0}; mesh.obb.axes[1]={0,1,0}; mesh.obb.axes[2]={0,0,1};
    zenkit::Material material;
    material.name="ARENA_STONE";
    material.group=zenkit::MaterialGroup::STONE;
    material.color={150,160,170,255};
    material.texture_scale={1,1};
    material.disable_lightmap=true;
    mesh.materials.push_back(material);
    auto quad=[&](std::array<zenkit::Vec3,4> v, zenkit::Vec3 normal) {
      const auto base=uint32_t(mesh.vertices.size());
      for(auto p:v) {
        mesh.vertices.push_back(p);
        mesh.features.push_back({{p.x/200.f,p.z/200.f},0xffffffff,normal});
      }
      // ZenGin uses clockwise faces; OpenGothic reverses them for Bullet.
      for(auto indices:{std::array<uint32_t,3>{0,2,1},std::array<uint32_t,3>{0,3,2}}) {
        zenkit::Polygon poly{};
        poly.lightmap=-1; poly.flags.sector_index=-1;
        poly.index_count=3; poly.index_offset=mesh.polygon_vertex_indices.size();
        poly.plane_normal=normal;
        poly.plane_distance=normal.x*v[0].x+normal.y*v[0].y+normal.z*v[0].z;
        for(auto i:indices) {
          mesh.polygon_vertex_indices.push_back(base+i);
          mesh.polygon_feature_indices.push_back(base+i);
          // Gothic II's writer reads the triangulated index arrays.
          mesh.polygons.vertex_indices.push_back(base+i);
          mesh.polygons.feature_indices.push_back(base+i);
        }
        mesh.geometry.push_back(poly);
      }
    };
    quad({{{-2000,0,-2000},{-2000,0,2000},{2000,0,2000},{2000,0,-2000}}},{0,1,0});
    quad({{{-2000,0,-2000},{2000,0,-2000},{2000,400,-2000},{-2000,400,-2000}}},{0,0,1});
    quad({{{2000,0,2000},{-2000,0,2000},{-2000,400,2000},{2000,400,2000}}},{0,0,-1});
    quad({{{-2000,0,2000},{-2000,0,-2000},{-2000,400,-2000},{-2000,400,2000}}},{1,0,0});
    quad({{{2000,0,-2000},{2000,0,2000},{2000,400,2000},{2000,400,-2000}}},{-1,0,0});
    auto& bsp=world.world_bsp_tree;
    bsp.mode=zenkit::BspTreeType::OUTDOOR;
    zenkit::BspNode leaf{};
    leaf.bbox=mesh.bbox; leaf.polygon_count=uint32_t(mesh.geometry.size());
    bsp.nodes.push_back(leaf); bsp.leaf_node_indices.push_back(0);
    bsp.light_points.push_back({0,200,0});
    for(uint32_t i=0;i<mesh.geometry.size();++i) bsp.polygon_indices.push_back(i);
    world.way_net=std::make_shared<zenkit::WayNet>();
    const std::array<zenkit::Vec3,6> positions={{{0,0,0},{300,0,0},{-1000,0,0},{1000,0,0},{0,0,-1000},{0,0,1000}}};
    for(size_t i=0;i<positions.size();++i) {
      auto point=std::make_shared<zenkit::WayPoint>();
      point->name=i==0 ? "START" : "ARENA_"+std::to_string(i);
      point->position=positions[i]; point->direction={0,0,1};
      point->water_depth=0; point->under_water=false;
      world.way_net->points.push_back(point);
      if(i) world.way_net->edges.emplace_back(world.way_net->points[0],point);
    }
    const std::filesystem::path output=argv[1];
    std::filesystem::create_directories(output.parent_path());
    {
      FixtureWriter stream(zenkit::Write::to(output));
      auto archive=zenkit::WriteArchive::to(&stream,zenkit::ArchiveFormat::BINARY);
      archive->write_object("%",&world,zenkit::GameVersion::GOTHIC_2);
      archive->write_header();
    }
    auto input=zenkit::Read::from(output);
    zenkit::World check;
    check.load(input.get(),zenkit::GameVersion::GOTHIC_2);
    std::cout<<"Round trip: "<<check.world_mesh.polygons.material_indices.size()<<" triangles, "
             <<(check.way_net ? check.way_net->points.size() : 0)<<" waypoints\n";
    if(check.world_mesh.polygons.material_indices.size()!=10 || !check.way_net || check.way_net->points.size()!=6)
      throw std::runtime_error("Arena round-trip failed");
    std::cout<<output<<": "<<std::filesystem::file_size(output)<<" bytes, 10 triangles, 6 waypoints\n";
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n'; return 1;
  }
}
