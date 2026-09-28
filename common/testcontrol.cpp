#include "mainwindow.h"
#include "commandline.h"
#include "gothic.h"
#include "marvin.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "network/fingerprint.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <random>
#include <chrono>
#include <cmath>

using namespace Tempest;
namespace fs = std::filesystem;

void MainWindow::initTestControl() {
  testControlDir = CommandLine::inst().testControlPath();
  if(testControlDir.empty())
    return;
  fs::create_directories(testControlDir);
  // Requests belong to this process lifetime; never replay an old action.
  fs::remove(fs::path(testControlDir)/"request.json");
  fs::remove(fs::path(testControlDir)/"response.json");
  testSessionId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+
                  "-"+std::to_string(std::random_device{}());
  {
    std::ofstream info(fs::path(testControlDir)/"session.tmp");
    info << "{\"session\":\"" << testSessionId << "\",\"protocol\":1}";
    info.close();
    if(!info)
      throw std::runtime_error("Cannot write test session identity");
    fs::rename(fs::path(testControlDir)/"session.tmp", fs::path(testControlDir)/"session.json");
  }
  Log::i("Development control enabled: ", testControlDir);
  }

void MainWindow::replyTestControl(std::string_view id, std::string_view error,
                                  std::string_view artifact) {
  rapidjson::StringBuffer data;
  rapidjson::Writer<rapidjson::StringBuffer> out(data);
  auto str = [&](std::string_view text) {
    out.String(text.empty() ? "" : text.data(), rapidjson::SizeType(text.size()));
    };
  auto& game = Gothic::inst();
  const bool idle = game.checkLoading()==Gothic::LoadState::Idle;
  // The loader owns the session while loading/saving. Do not inspect it then.
  auto world = idle ? game.world() : nullptr;
  auto npc = world ? game.player() : nullptr;
  out.StartObject();
  out.Key("id"); str(id);
  out.Key("session"); str(testSessionId);
  out.Key("ok"); out.Bool(error.empty());
  out.Key("error"); str(error);
  out.Key("artifact"); str(artifact);
  out.Key("loading"); out.Bool(!idle);
  out.Key("ready"); out.Bool(npc!=nullptr);
  out.Key("save_status"); str(saveStatus);
  out.Key("load_status"); str(loadStatus);
  out.Key("dialog"); out.Bool(dialogs.isActive());
  out.Key("video"); out.Bool(video.isActive());
  out.Key("menu"); out.Bool(rootMenu.isActive());
  out.Key("chapter"); out.Bool(chapter.isActive());
  out.Key("inventory"); out.Bool(inventory.isActive());
  const auto& net = network.status();
  out.Key("network"); out.StartObject();
  out.Key("role"); str(CommandLine::inst().coopEndpoint().role==Coop::Role::Host ? "host" :
                       CommandLine::inst().coopEndpoint().role==Coop::Role::Guest ? "guest" : "disabled");
  out.Key("state"); str(networkPreparation.empty() ? net.state : networkPreparation);
  out.Key("reason"); str(net.reason);
  out.Key("port"); out.Uint(net.port);
  out.Key("protocol"); out.Uint(Coop::ProtocolVersion);
  out.Key("generation"); out.Uint64(net.generation);
  out.Key("bound_player"); out.Uint64(net.boundPlayer);
  out.Key("binding_revision"); out.Uint64(net.bindingRevision);
  out.Key("bindings"); out.Uint64(net.bindings);
  out.Key("peers"); out.Uint64(net.peers);
  out.Key("accepted"); out.Uint64(net.accepted);
  out.Key("rejected"); out.Uint64(net.rejected);
  out.Key("timeouts"); out.Uint64(net.timeouts);
  out.Key("disconnected"); out.Uint64(net.disconnected);
  out.Key("engine_fingerprint"); str(Coop::hexFingerprint(networkIdentity.engine));
  out.Key("data_fingerprint"); str(Coop::hexFingerprint(networkIdentity.gameData));
  out.EndObject();
  if(world) {
    out.Key("world"); str(world->name());
    out.Key("tick"); out.Uint64(world->tickCount());
    out.Key("npc_count"); out.Uint(world->npcCount());
    const auto& registry = world->objectRegistry();
    out.Key("object_registry"); out.StartObject();
    out.Key("namespace"); out.Uint64(registry.worldNamespace());
    out.Key("next_id"); out.Uint64(registry.nextId());
    size_t npcs=0, items=0;
    out.Key("removed"); out.StartArray();
    for(const auto& [serial,record]:registry.entries()) {
      if(record.object) {
        if(record.kind==Coop::ObjectKind::Npc) ++npcs; else ++items;
        continue;
        }
      out.StartObject();
      out.Key("id"); out.Uint64(serial);
      out.Key("kind"); str(record.kind==Coop::ObjectKind::Npc ? "npc" : "item");
      out.EndObject();
      }
    out.EndArray();
    out.Key("live_npcs"); out.Uint64(npcs);
    out.Key("live_items"); out.Uint64(items);
    out.EndObject();
    }
  if(npc) {
    auto vec = [&](const Vec3& v) {
      out.StartArray(); out.Double(v.x); out.Double(v.y); out.Double(v.z); out.EndArray();
      };
    out.Key("story_player_id"); out.Uint64(world->playerId(npc));
    out.Key("local_player_id"); out.Uint64(game.localPlayerId());
    out.Key("input_player_id"); out.Uint64(world->playerId(player.controlledPlayer()));
    out.Key("hud"); out.StartObject();
    out.Key("player"); out.Uint64(paintedHud.player);
    out.Key("hp"); out.Int(paintedHud.hp);
    out.Key("hp_max"); out.Int(paintedHud.hpMax);
    out.Key("mana"); out.Int(paintedHud.mana);
    out.Key("mana_max"); out.Int(paintedHud.manaMax);
    out.EndObject();
    const auto p = npc->position();
    out.Key("position"); out.StartArray();
    out.Double(p.x); out.Double(p.y); out.Double(p.z); out.EndArray();
    out.Key("rotation"); out.Double(npc->rotation());
    out.Key("hp"); out.Int(npc->attribute(ATR_HITPOINTS));
    out.Key("hp_max"); out.Int(npc->attribute(ATR_HITPOINTSMAX));
    auto itemInfo = [&](const Item& item, size_t count) {
      out.StartObject();
      out.Key("object_id"); out.Uint64(world->objectId(&item));
      const auto index = world->itmId(&item.handle());
      out.Key("world_index"); out.Int64(index==uint32_t(-1) ? -1 : int64_t(index));
      out.Key("symbol");
      auto sym = world->script().findSymbol(item.clsId());
      str(sym ? sym->name() : "");
      out.Key("name"); str(item.displayName());
      out.Key("count"); out.Uint64(count);
      out.Key("equipped"); out.Bool(item.isEquipped());
      out.EndObject();
      };
    out.Key("items"); out.StartArray();
    for(auto it = npc->inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
      itemInfo(*it, it.count());
    out.EndArray();
    out.Key("inventory_player_id"); out.Uint64(world->playerId(inventory.activePlayer()));
    out.Key("inventory_items"); out.StartArray();
    if(auto owner = inventory.activePlayer())
      for(auto it=owner->inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
        itemInfo(*it,it.count());
    out.EndArray();
    out.Key("players"); out.StartArray();
    for(auto& entry:world->players()) {
      auto actor = entry.second;
      auto pos = actor->position();
      out.StartObject();
      out.Key("id"); out.Uint64(entry.first);
      out.Key("object_id"); out.Uint64(world->objectId(actor));
      out.Key("npc_index"); out.Uint(world->npcId(actor)); // Diagnostic only; not a persistent ID.
      out.Key("position"); out.StartArray();
      out.Double(pos.x); out.Double(pos.y); out.Double(pos.z); out.EndArray();
      out.Key("rotation"); out.Double(actor->rotation());
      out.Key("hp"); out.Int(actor->attribute(ATR_HITPOINTS));
      out.Key("hp_max"); out.Int(actor->attribute(ATR_HITPOINTSMAX));
      out.Key("camera_bone"); vec(actor->cameraBone(game.camera()->isFirstPerson()));
      Vec3 eye(0,0,20);
      actor->cameraMatrix(true).project(eye);
      out.Key("first_person_eye"); vec(eye);
      out.Key("controlled"); out.Bool(actor->isPlayer());
      out.Key("focus_symbol");
      auto actorFocus = world->findFocus(*actor,Focus());
      auto focusedItem = actorFocus.item ? world->script().findSymbol(actorFocus.item->clsId()) : nullptr;
      str(focusedItem ? focusedItem->name() : "");
      out.Key("items"); out.StartArray();
      for(auto it=actor->inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
        itemInfo(*it,it.count());
      out.EndArray();
      out.EndObject();
      }
    out.EndArray();
    out.Key("hero_is_host");
    auto heroSymbol = world->script().findSymbol("HERO");
    auto host = world->playerById(1);
    out.Bool(host && heroSymbol && heroSymbol->get_instance().get()==&host->handle());
    out.Key("hero_template_is_host");
    auto heroTemplate = world->script().findSymbol("PC_HERO");
    out.Bool(host && heroTemplate && heroTemplate->get_instance().get()==&host->handle());
    out.Key("nearby_items"); out.StartArray();
    world->detectItem(p, 300.f, [&](Item& item) { itemInfo(item, item.count()); });
    out.EndArray();
    const auto focus = player.focus();
    out.Key("focus"); out.StartObject();
    out.Key("type"); str(focus.item ? "item" : focus.npc ? "npc" : focus.interactive ? "interactive" : "none");
    out.Key("name"); str(focus.displayName());
    if(focus.item) {
      out.Key("symbol");
      auto sym = world->script().findSymbol(focus.item->clsId());
      str(sym ? sym->name() : "");
      }
    out.EndObject();
    if(auto camera = game.camera()) {
      out.Key("camera_target"); vec(camera->destTarget());
      out.Key("camera_origin"); vec(camera->originLwc());
      out.Key("camera_first_person"); out.Bool(camera->isFirstPerson());
      out.Key("camera_spin"); out.StartArray();
      out.Double(camera->spin().x); out.Double(camera->spin().y); out.EndArray();
      }
    }
  out.EndObject();
  const auto tmp = fs::path(testControlDir)/"response.tmp";
  std::ofstream file(tmp, std::ios::binary|std::ios::trunc);
  file.write(data.GetString(), std::streamsize(data.GetSize()));
  file.close();
  if(!file)
    throw std::runtime_error("Cannot write test response");
  fs::rename(tmp, fs::path(testControlDir)/"response.json");
  }

void MainWindow::pollTestControl() {
  if(testControlDir.empty())
    return;
  const auto now = Application::tickCount();
  if(now<testPollAt)
    return;
  testPollAt = now+25;
  try {
    if(!testHeldKeys.empty()) {
      if(now<testReleaseAt)
        return;
      for(auto it=testHeldKeys.rbegin(); it!=testHeldKeys.rend(); ++it) {
        KeyEvent up(*it, Event::M_NoModifier, Event::KeyUp);
        dispatchPlayerKey(testInputPlayer, up, false);
        }
      testHeldKeys.clear();
      replyTestControl(testRequestId);
      return;
      }
    const auto request = fs::path(testControlDir)/"request.json";
    if(!fs::exists(request))
      return;
    if(fs::file_size(request)>16384) {
      fs::remove(request);
      throw std::runtime_error("Test request exceeds 16 KiB");
      }
    std::ifstream file(request);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    file.close();
    fs::remove(request);
    testRequestId.clear();
    rapidjson::Document cmd;
    cmd.Parse(text.c_str());
    if(cmd.HasParseError() || !cmd.IsObject() || !cmd.HasMember("id") ||
       !cmd["id"].IsString() || !cmd.HasMember("action") || !cmd["action"].IsString())
      throw std::runtime_error("Expected id and action strings");
    testRequestId = cmd["id"].GetString();
    if(testRequestId.empty() || testRequestId.size()>64 ||
       testRequestId.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_")!=std::string::npos) {
      testRequestId.clear();
      throw std::runtime_error("Invalid request id");
      }
    const std::string_view action = cmd["action"].GetString();
    if(!cmd.HasMember("session") || !cmd["session"].IsString() ||
       std::string_view(cmd["session"].GetString())!=testSessionId)
      throw std::runtime_error("Test session changed; stale command rejected");
    auto& game = Gothic::inst();
    if(action=="state") {
      replyTestControl(testRequestId);
      return;
      }
    if(game.checkLoading()!=Gothic::LoadState::Idle)
      throw std::runtime_error("Game is loading or saving");
    if(action=="quit") {
      clearInput();
      replyTestControl(testRequestId);
      SystemApi::exit();
      return;
      }
    if(!game.player())
      throw std::runtime_error("No player is ready");
    testInputPlayer = game.localPlayerId();
    if(cmd.HasMember("player")) {
      if(!cmd["player"].IsUint64() || !game.world()->playerById(cmd["player"].GetUint64()))
        throw std::runtime_error("Unknown player identity");
      testInputPlayer = cmd["player"].GetUint64();
      }
    if(action=="key" || action=="chord") {
      if(!cmd.HasMember("ms") || !cmd["ms"].IsUint() || cmd["ms"].GetUint()>5000)
        throw std::runtime_error("Expected a key and duration in 0..5000 ms");
      std::vector<std::string_view> names;
      if(action=="key" && cmd.HasMember("key") && cmd["key"].IsString())
        names.emplace_back(cmd["key"].GetString());
      else if(action=="chord" && cmd.HasMember("keys") && cmd["keys"].IsArray()) {
        for(auto& key:cmd["keys"].GetArray()) {
          if(!key.IsString())
            throw std::runtime_error("Key names must be strings");
          names.emplace_back(key.GetString());
          }
        }
      if(names.empty() || names.size()>4)
        throw std::runtime_error("Expected 1..4 keys");
      struct Mapping { std::string_view name; Event::KeyType key; };
      static const Mapping keys[] = {
        {"UP",Event::K_Up}, {"DOWN",Event::K_Down},
        {"LEFT",Event::K_Left}, {"RIGHT",Event::K_Right},
        {"W",Event::K_W}, {"A",Event::K_A}, {"S",Event::K_S}, {"D",Event::K_D},
        {"SPACE",Event::K_Space}, {"CTRL",Event::K_LControl}, {"ALT",Event::K_LAlt},
        {"TAB",Event::K_Tab}, {"ESC",Event::K_ESCAPE}, {"ENTER",Event::K_Return},
        {"F",Event::K_F}, {"B",Event::K_B}
        };
      for(auto name:names) {
        auto found = Event::K_NoKey;
        for(auto& k:keys)
          if(name==k.name)
            found = k.key;
        if(found==Event::K_NoKey || std::find(testHeldKeys.begin(),testHeldKeys.end(),found)!=testHeldKeys.end())
          throw std::runtime_error("Unsupported or duplicate test key");
        testHeldKeys.push_back(found);
        }
      testReleaseAt = now+cmd["ms"].GetUint();
      for(auto key:testHeldKeys) {
        KeyEvent down(key);
        dispatchPlayerKey(testInputPlayer, down, true);
        }
      return; // Reply after releasing the key; rendering/simulation keep running.
      }
    if(action=="select_local_player") {
      if(!cmd.HasMember("player"))
        throw std::runtime_error("Expected a local player identity");
      selectLocalPlayer(testInputPlayer);
      }
    else if(action=="mouse") {
      if(testInputPlayer!=game.localPlayerId() || !cmd.HasMember("dx") || !cmd.HasMember("dy") ||
         !cmd["dx"].IsInt() || !cmd["dy"].IsInt() ||
         cmd["dx"].GetInt()<-1000 || cmd["dx"].GetInt()>1000 ||
         cmd["dy"].GetInt()<-1000 || cmd["dy"].GetInt()>1000)
        throw std::runtime_error("Expected local mouse deltas in -1000..1000 pixels");
      // Feed the ordinary mouse processing step; it updates camera and controller.
      dMouse += Point(cmd["dx"].GetInt(),cmd["dy"].GetInt());
      }
    else if(action=="fixture_health") {
      auto actor = game.world()->playerById(testInputPlayer);
      if(!cmd.HasMember("hp") || !cmd["hp"].IsInt() || cmd["hp"].GetInt()<1 ||
         cmd["hp"].GetInt()>actor->attribute(ATR_HITPOINTSMAX))
        throw std::runtime_error("Expected fixture HP in 1..maximum HP");
      actor->changeAttribute(ATR_HITPOINTS,cmd["hp"].GetInt()-actor->attribute(ATR_HITPOINTS),false);
      }
    else if(action=="add_companion") {
      game.world()->addPlayer(2);
      controlForPlayer(2);
      }
    else if(action=="fixture_plant") {
      // Setup only: create a world item. Pickup must still use normal input.
      auto npc = game.world()->playerById(testInputPlayer);
      auto pos = npc->position();
      pos.x += 70.f*std::cos(npc->rotationRad());
      pos.z += 70.f*std::sin(npc->rotationRad());
      auto ground = game.world()->physic()->landRay(pos+Vec3(0,200,0),400);
      if(!ground.hasCol)
        throw std::runtime_error("No ground for plant fixture");
      pos.y = ground.v.y+5.f;
      const auto cls = game.world()->script().findSymbolIndex("ITPL_HEALTH_HERB_01");
      if(cls==size_t(-1) || !game.world()->addItem(cls,pos))
        throw std::runtime_error("Cannot create plant fixture");
      }
    else if(action=="remove_world_item") {
      if(!cmd.HasMember("object_id") || !cmd["object_id"].IsUint64() ||
         !cmd.HasMember("namespace") || !cmd["namespace"].IsUint64())
        throw std::runtime_error("Expected an object namespace and ID");
      auto item = game.world()->itemByObjectId(cmd["namespace"].GetUint64(),cmd["object_id"].GetUint64());
      if(!item) throw std::runtime_error("Unknown or removed world item identity");
      game.world()->removeItem(*item);
      }
    else if(action=="console") {
      if(!cmd.HasMember("command") || !cmd["command"].IsString())
        throw std::runtime_error("Expected console command");
      Marvin marvin;
      if(!marvin.exec(cmd["command"].GetString()))
        throw std::runtime_error("Console command failed");
      }
    else if(action=="screenshot" || action=="screenshot_ui") {
      Attachment tex;
      if(action=="screenshot_ui") {
        device.waitIdle();
        dispatchPaintEvent(uiLayer,atlas);
        numOverlay.clear();
        PaintEvent paint(numOverlay,atlas,w(),h());
        inventory.paintNumOverlay(paint);
        uiMesh[cmdId].update(device,uiLayer);
        numMesh[cmdId].update(device,numOverlay);
        tex = device.attachment(TextureFormat::RGBA8,swapchain.w(),swapchain.h());
        CommandBuffer buffer;
        {
        auto enc = buffer.startEncoding(device);
        renderer.draw(tex,enc,cmdId,uiMesh[cmdId],numMesh[cmdId],inventory,video);
        }
        auto sync = device.submit(buffer);
        sync.wait();
        }
      else {
        tex = renderer.screenshoot(cmdId);
        }
      if(tex.isEmpty())
        throw std::runtime_error("No rendered scene");
      const auto output = fs::path(testControlDir)/("capture-"+testRequestId+".png");
      device.readPixels(textureCast<const Texture2d&>(tex)).save(output.string().c_str());
      replyTestControl(testRequestId, {}, output.string());
      return;
      }
    else if(action=="load") {
      loadGame("save_slot_99.sav");
      }
    else if(action=="save") {
      if(dialogs.isActive() || game.world()->currentCs()!=nullptr)
        throw std::runtime_error("Cannot save during dialogue or a cutscene");
      saveGame("save_slot_99.sav", "Automation test checkpoint");
      // This starts the async save; the caller must wait for idle and verify disk.
      }
    else
      throw std::runtime_error("Unknown test action");
    replyTestControl(testRequestId);
    }
  catch(const std::exception& e) {
    clearInput();
    testHeldKeys.clear();
    Log::e("Test control: ", e.what());
    try { replyTestControl(testRequestId, e.what()); }
    catch(const std::exception& io) { Log::e("Test response: ", io.what()); }
    }
  }
