#include "mainwindow.h"
#include "commandline.h"
#include "gothic.h"
#include "marvin.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "network/fingerprint.h"
#include "game/gamesession.h"
#include "world/triggers/abstracttrigger.h"
#include "game/serialize.h"

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
  out.Key("authority"); str(game.isGuest() ? "guest" : "campaign");
  out.Key("waiting_snapshot"); out.Bool(game.isGuest() && idle && !world && loadStatus!="failed");
  out.Key("rendered_frames"); out.Uint64(renderedFrames);
  out.Key("simulation_held"); out.Bool(runtimeMode==R_Suspended);
  out.Key("save_status"); str(saveStatus);
  const auto boundary = [&](const std::optional<CheckpointBoundary>& value) {
    if(!value) { out.Null(); return; }
    out.StartObject();
    out.Key("world"); out.Uint64(value->world);
    out.Key("tick"); out.Uint64(value->tick);
    out.Key("generation"); out.Uint64(value->generation);
    out.Key("revision"); out.Uint64(value->revision);
    out.Key("input"); out.Uint64(value->input);
    out.EndObject();
    };
  out.Key("save_boundary"); boundary(saveBoundary);
  out.Key("loaded_checkpoint"); boundary(idle && game.gameSession() ? game.gameSession()->checkpointBoundary() : std::nullopt);
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
  out.Key("snapshot_state"); str(net.snapshotState);
  out.Key("snapshot_revision"); out.Uint64(net.snapshotRevision);
  out.Key("snapshot_bytes"); out.Uint64(net.snapshotBytes);
  out.Key("snapshot_transferred"); out.Uint64(net.snapshotTransferred);
  out.Key("snapshot_digest"); str(Coop::hexFingerprint(net.snapshotDigest));
  out.Key("snapshot_tick"); out.Uint64(networkSnapshotTick);
  out.Key("snapshot_artifact"); str(networkSnapshotArtifact);
  out.Key("paused"); out.Bool(game.isNetworkPaused());
  out.Key("input_sent"); out.Uint64(net.inputSent);
  out.Key("input_received"); out.Uint64(net.inputReceived);
  out.Key("input_applied"); out.Uint64(net.inputApplied);
  out.Key("input_held"); out.Bool(net.inputHeld);
  out.Key("input_acknowledged"); out.Uint64(net.inputAcknowledged);
  out.Key("input_duplicates"); out.Uint64(net.inputDuplicates);
  out.Key("input_stale"); out.Uint64(net.inputStale);
  out.Key("pickup_requests"); out.Uint64(networkPickupRequests);
  out.Key("pickup_accepted"); out.Uint64(networkPickupAccepted);
  out.Key("equipment_requests"); out.Uint64(networkEquipmentRequests);
  out.Key("equipment_accepted"); out.Uint64(networkEquipmentAccepted);
  out.Key("acknowledged_revision"); out.Uint64(net.acknowledgedRevision);
  out.Key("held_buttons"); out.Uint(networkButtons);
  out.Key("remote_buttons"); out.Uint(remoteButtons);
  out.Key("can_rejoin"); out.Bool(canRejoinNetwork());
  out.Key("poses_sent"); out.Uint64(net.posesSent);
  out.Key("poses_received"); out.Uint64(net.posesReceived);
  out.Key("poses_applied"); out.Uint64(net.posesApplied);
  out.Key("poses_acknowledged"); out.Uint64(net.posesAcknowledged);
  out.Key("pose_tick"); out.Uint64(net.poseTick);
  out.Key("interpolation_ms"); out.Uint64(Coop::PoseInterpolation::DurationMs);
  out.Key("interpolation_alpha"); out.Double(networkInterpolation.alpha());
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
    out.Key("presentation_tick"); out.Uint64(world->presentationTickCount());
    out.Key("script_function_calls"); out.Uint64(world->script().functionCalls());
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
    auto combatInfo = [&](const Npc& actor) {
      out.Key("dead"); out.Bool(actor.isDead());
      out.Key("unconscious"); out.Bool(actor.isUnconscious());
      out.Key("weapon_state"); out.Int(int(actor.weaponState()));
      out.Key("level"); out.Int(actor.level());
      out.Key("xp"); out.Int(actor.experience());
      out.Key("xp_next"); out.Int(actor.experienceNext());
      out.Key("lp"); out.Int(actor.learningPoints());
      out.Key("last_hit_id"); out.Uint64(world->objectId(actor.lastHitNpc()));
      out.Key("target_id"); out.Uint64(world->objectId(actor.target()));
      };
    out.Key("combat_constants"); out.StartObject();
    for(const auto name : {"XP_PER_VICTORY", "HP_PER_LEVEL", "LP_PER_LEVEL"}) {
      out.Key(name);
      if(auto sym = world->script().findSymbol(name)) out.Int(sym->get_int()); else out.Null();
      }
    out.EndObject();
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
    auto itemInfo = [&](const Item& item, size_t count, bool equipped=false, uint8_t slot=Item::NSLOT) {
      out.StartObject();
      out.Key("object_id"); out.Uint64(world->objectId(&item));
      out.Key("instance"); out.Uint64(item.clsId());
      const auto index = world->itmId(&item.handle());
      out.Key("world_index"); out.Int64(index==uint32_t(-1) ? -1 : int64_t(index));
      out.Key("symbol");
      auto sym = world->script().findSymbol(item.clsId());
      str(sym ? sym->name() : "");
      out.Key("name"); str(item.displayName());
      out.Key("count"); out.Uint64(count);
      out.Key("equipped"); out.Bool(equipped);
      out.Key("slot"); out.Uint(slot);
      out.Key("weapon"); out.Bool(item.isEquipmentWeapon());
      out.Key("usable_by_local_player"); int32_t attribute=0,value=0; out.Bool(item.checkCondUse(*game.localPlayer(),attribute,value));
      out.EndObject();
      };
    out.Key("items"); out.StartArray();
    for(auto it = npc->inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
      itemInfo(*it, it.count(),it.isEquipped(),it.slot());
    out.EndArray();
    out.Key("inventory_player_id"); out.Uint64(world->playerId(inventory.activePlayer()));
    out.Key("inventory_selected"); out.Uint64(inventory.selectedItemInstance());
    out.Key("inventory_selected_equipped"); out.Bool(inventory.selectedItemEquipped());
    out.Key("inventory_items"); out.StartArray();
    if(auto owner = inventory.activePlayer())
      for(auto it=owner->inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
        itemInfo(*it,it.count(),it.isEquipped(),it.slot());
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
      out.Key("animation_layers"); out.StartArray();
      for(auto& layer:actor->networkPose().layers) {
        out.StartObject();
        out.Key("name"); str(layer.name);
        out.Key("started"); out.Uint64(layer.started);
        out.Key("body"); out.Uint(layer.body);
        out.Key("combination"); out.Uint(layer.combination);
        out.EndObject();
        }
      out.EndArray();
      out.Key("hp"); out.Int(actor->attribute(ATR_HITPOINTS));
      out.Key("hp_max"); out.Int(actor->attribute(ATR_HITPOINTSMAX));
      combatInfo(*actor);
      out.Key("camera_bone"); vec(actor->cameraBone(game.camera()->isFirstPerson()));
      out.Key("head_bone"); vec(actor->cameraBone(true));
      Vec3 eye(0,0,20);
      actor->cameraMatrix(true).project(eye);
      out.Key("first_person_eye"); vec(eye);
      out.Key("controlled"); out.Bool(actor->isPlayer());
      out.Key("focus_symbol");
      auto actorFocus = world->findFocus(*actor,Focus());
      auto focusedItem = actorFocus.item ? world->script().findSymbol(actorFocus.item->clsId()) : nullptr;
      str(focusedItem ? focusedItem->name() : "");
      out.Key("focus_item_id"); out.Uint64(actorFocus.item ? world->objectId(actorFocus.item) : 0);
      out.Key("focus_npc_id"); out.Uint64(actorFocus.npc ? world->objectId(actorFocus.npc) : 0);
      const auto equipment = actor->networkEquipment();
      const auto views = actor->networkWeaponViews();
      out.Key("equipment"); out.StartObject();
      out.Key("melee"); out.Uint64(equipment.melee);
      out.Key("ranged"); out.Uint64(equipment.ranged);
      out.Key("melee_view"); out.Bool(views[0]);
      out.Key("ranged_view"); out.Bool(views[1]);
      out.EndObject();
      out.Key("items"); out.StartArray();
      for(auto it=actor->inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
        itemInfo(*it,it.count(),it.isEquipped(),it.slot());
      out.EndArray();
      out.EndObject();
      }
    out.EndArray();
    out.Key("nearby_npcs"); out.StartArray();
    for(uint32_t i=0; i<world->npcCount(); ++i) {
      const auto other = world->npcById(i);
      if(!other || world->playerId(other)!=0) continue;
      bool nearby = false;
      for(const auto& entry:world->players())
        nearby |= other->qDistTo(*entry.second)<2000.f*2000.f;
      if(!nearby) continue;
      out.StartObject();
      out.Key("object_id"); out.Uint64(world->objectId(other));
      const auto symbol = world->script().findSymbol(other->instanceSymbol());
      out.Key("symbol"); str(symbol ? symbol->name() : "");
      out.Key("position"); vec(other->position());
      out.Key("rotation"); out.Double(other->rotation());
      out.Key("hp"); out.Int(other->attribute(ATR_HITPOINTS));
      out.Key("hp_max"); out.Int(other->attribute(ATR_HITPOINTSMAX));
      combatInfo(*other);
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
      if(now>=testReleaseAt) {
        for(auto it=testHeldKeys.rbegin(); it!=testHeldKeys.rend(); ++it) {
          KeyEvent up(*it, Event::M_NoModifier, Event::KeyUp);
          dispatchPlayerKey(testHeldPlayer, up, false);
          }
        testHeldKeys.clear();
        if(!testAsyncInput) {
          replyTestControl(testRequestId);
          return;
          }
        testAsyncInput = false;
        }
      else if(!testAsyncInput) return;
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
    const bool reconnectKey = game.isGuest() && action=="key" && cmd.HasMember("key") &&
                              cmd["key"].IsString() && std::string_view(cmd["key"].GetString())=="R";
    if(!game.player() && action!="screenshot_ui" && action!="start_game" && !reconnectKey)
      throw std::runtime_error("No player is ready");
    if(game.isNetworkPaused() && action!="key" && action!="chord" && action!="hold_keys" && action!="release_keys" && action!="mouse" &&
       action!="screenshot" && action!="screenshot_ui" && action!="campaign_probe" && action!="save")
      throw std::runtime_error("Campaign held at snapshot boundary");
    testInputPlayer = game.localPlayerId();
    if(cmd.HasMember("player")) {
      if(!cmd["player"].IsUint64() || game.world()==nullptr || !game.world()->playerById(cmd["player"].GetUint64()))
        throw std::runtime_error("Unknown player identity");
      testInputPlayer = cmd["player"].GetUint64();
      }
    if(action=="release_keys") {
      for(auto key:testHeldKeys) {
        KeyEvent up(key, Event::M_NoModifier, Event::KeyUp);
        dispatchPlayerKey(testHeldPlayer,up,false);
        }
      testHeldKeys.clear();
      testAsyncInput = false;
      replyTestControl(testRequestId);
      return;
      }
    if(action=="key" || action=="chord" || action=="hold_keys") {
      if(!testHeldKeys.empty()) throw std::runtime_error("Test keys are already held");
      if(!cmd.HasMember("ms") || !cmd["ms"].IsUint() || cmd["ms"].GetUint()>5000)
        throw std::runtime_error("Expected a key and duration in 0..5000 ms");
      std::vector<std::string_view> names;
      if(action=="key" && cmd.HasMember("key") && cmd["key"].IsString())
        names.emplace_back(cmd["key"].GetString());
      else if((action=="chord" || action=="hold_keys") && cmd.HasMember("keys") && cmd["keys"].IsArray()) {
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
        {"F",Event::K_F}, {"B",Event::K_B}, {"R",Event::K_R}
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
      testHeldPlayer = testInputPlayer;
      testAsyncInput = action=="hold_keys";
      for(auto key:testHeldKeys) {
        KeyEvent down(key);
        dispatchPlayerKey(testInputPlayer, down, true);
        }
      if(testAsyncInput) replyTestControl(testRequestId);
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
    else if(action=="hold_simulation") {
      if(!cmd.HasMember("hold") || !cmd["hold"].IsBool())
        throw std::runtime_error("Expected hold boolean");
      clearInput();
      runtimeMode = cmd["hold"].GetBool() ? R_Suspended : R_Normal;
      }
    else if(action=="step_simulation") {
      if(!game.world()->isAuthoritative() || runtimeMode!=R_Suspended)
        throw std::runtime_error("Step requires a held authoritative simulation");
      // One ordinary simulation/controller step, retaining queued real input.
      runtimeMode = R_Step;
      }
    else if(action=="campaign_probe") {
      // Read-only diagnostic archive, not a loadable/saveable guest checkpoint.
      const auto output = fs::path(testControlDir)/("campaign-"+testRequestId+".zip");
      {
      WFile file(output.string().c_str());
      Serialize snapshot(file);
      game.world()->save(snapshot);
      snapshot.setEntry("game/daedalus");
      game.world()->script().saveVar(snapshot);
      snapshot.setEntry("game/quests");
      game.world()->script().saveQuests(snapshot);
      snapshot.setEntry("game/perc");
      game.world()->script().savePerc(snapshot);
      snapshot.finish();
      }
      replyTestControl(testRequestId, {}, output.string());
      return;
      }
    else if(action=="attempt_campaign_effects") {
      // Deliberately bypass the UI to exercise real engine authority gates.
      auto world = game.world();
      auto actor = world->playerById(testInputPlayer);
      actor->changeAttribute(ATR_HITPOINTS,-7,false);
      Item* plant = nullptr;
      const auto cls = world->script().findSymbolIndex("ITPL_HEALTH_HERB_01");
      world->detectItem(actor->position(),300.f,[&](Item& item) {
        if(item.clsId()==cls) plant = &item;
        });
      if(!plant) throw std::runtime_error("Missing plant for campaign effects probe");
      actor->takeItem(*plant);
      world->triggerEvent(TriggerEvent("N07_PROBE","",world->tickCount()+60000,TriggerEvent::T_Trigger));
      world->script().runFunction("INITPERCEPTIONS");
      game.gameSession()->tick(16);
      }
    else if(action=="fixture_quest") {
      auto& script = game.world()->script();
      script.log_createtopic("N07 isolation",0);
      script.log_addentry("N07 isolation","Host checkpoint marker");
      }
    else if(action=="start_game") {
      startGame(game.defaultWorld());
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
    else if(action=="fixture_shared_plants") {
      auto first = game.world()->playerById(1);
      auto second = game.world()->playerById(2);
      if(!game.world()->isAuthoritative() || !first || !second)
        throw std::runtime_error("Shared fixture requires two authoritative players");
      const auto cls = game.world()->script().findSymbolIndex("ITPL_HEALTH_HERB_01");
      for(const auto distance : {150.f,190.f}) {
        auto pos = (first->position()+second->position())*0.5f;
        pos.x += distance*std::cos(first->rotationRad());
        pos.z += distance*std::sin(first->rotationRad());
        auto ground = game.world()->physic()->landRay(pos+Vec3(0,200,0),400);
        if(!ground.hasCol) throw std::runtime_error("No ground for shared plant fixture");
        pos.y = ground.v.y+5.f;
        if(cls==size_t(-1) || !game.world()->addItem(cls,pos))
          throw std::runtime_error("Cannot create shared plant fixture");
        }
      }
    else if(action=="fixture_weapons") {
      if(!game.world()->isAuthoritative()) throw std::runtime_error("Guest cannot create weapon fixture");
      auto npc = game.world()->playerById(testInputPlayer);
      for(const auto symbol : {"ITMW_1H_VLK_DAGGER","ITMW_1H_BAU_MACE","ITRW_BOW_L_01","ITMW_1H_VLK_SWORD"}) {
        auto cls = game.world()->script().findSymbolIndex(symbol);
        if(cls==size_t(-1)) throw std::runtime_error("Missing weapon fixture instance");
        npc->addItem(cls,std::string_view(symbol)=="ITMW_1H_VLK_DAGGER" ? 2 : 1);
        }
      }
    else if(action=="fixture_combat_position") {
      auto world = game.world();
      if(!world->isAuthoritative()) throw std::runtime_error("Guest cannot position combat fixture");
      if(!cmd.HasMember("namespace") || !cmd["namespace"].IsUint64() ||
         !cmd.HasMember("object_id") || !cmd["object_id"].IsUint64() ||
         !cmd.HasMember("distance") || !cmd["distance"].IsNumber() ||
         !cmd.HasMember("bearing") || !cmd["bearing"].IsNumber())
        throw std::runtime_error("Expected NPC identity, distance and bearing");
      auto target = world->npcByObjectId(cmd["namespace"].GetUint64(),cmd["object_id"].GetUint64());
      auto actor = world->playerById(testInputPlayer);
      const double distance = cmd["distance"].GetDouble(), bearing = cmd["bearing"].GetDouble();
      if(!target || target==actor || actor->isDown() || !std::isfinite(distance) ||
         distance<80 || distance>1000 || !std::isfinite(bearing) || bearing<-360 || bearing>360)
        throw std::runtime_error("Invalid combat fixture position");
      auto pos = target->position()+Vec3(float(distance*std::cos(bearing*M_PI/180.0)),0,
                                       float(distance*std::sin(bearing*M_PI/180.0)));
      const auto ground = world->physic()->landRay(pos+Vec3(0,200,0),400);
      if(!ground.hasCol) throw std::runtime_error("No ground for combat fixture position");
      pos.y = ground.v.y;
      clearInput();
      actor->stopWalking();
      if(!actor->setPosition(pos)) throw std::runtime_error("Cannot position combat fixture player");
      actor->setDirection(target->position()-pos);
      }
    else if(action=="fixture_monster") {
      auto world = game.world();
      if(!world->isAuthoritative()) throw std::runtime_error("Guest cannot create monster fixture");
      if(!cmd.HasMember("mode") || !cmd["mode"].IsString())
        throw std::runtime_error("Expected calm or aggressive monster mode");
      const std::string_view mode = cmd["mode"].GetString();
      if(mode!="calm" && mode!="aggressive")
        throw std::runtime_error("Expected calm or aggressive monster mode");
      // The fixed scavenger fixture has 70 maximum HP in the installed scripts.
      // Lower starting health shortens tests; combat must still cause all damage.
      int hp = 20;
      if(cmd.HasMember("hp")) {
        if(!cmd["hp"].IsInt() || cmd["hp"].GetInt()<1 || cmd["hp"].GetInt()>70)
          throw std::runtime_error("Expected monster starting HP in 1..70");
        hp = cmd["hp"].GetInt();
        }
      float distance = 150.f;
      if(cmd.HasMember("distance")) {
        if(!cmd["distance"].IsNumber() || !std::isfinite(cmd["distance"].GetDouble()) ||
           cmd["distance"].GetDouble()<80 || cmd["distance"].GetDouble()>1000)
          throw std::runtime_error("Expected monster distance in 80..1000 world units");
        distance = float(cmd["distance"].GetDouble());
        }
      auto actor = world->playerById(testInputPlayer);
      if(actor->isDown()) throw std::runtime_error("Monster fixture requires a standing player");
      const auto cls = world->script().findSymbolIndex("SCAVENGER");
      const auto attack = world->script().findSymbolIndex("ZS_MM_ATTACK");
      if(cls==size_t(-1) || attack==size_t(-1))
        throw std::runtime_error("Missing original monster fixture scripts");
      auto pos = actor->position();
      pos.x += distance*std::cos(actor->rotationRad());
      pos.z += distance*std::sin(actor->rotationRad());
      const auto ground = world->physic()->landRay(pos+Vec3(0,200,0),400);
      if(!ground.hasCol) throw std::runtime_error("No ground for monster fixture");
      pos.y = ground.v.y;
      auto monster = world->addNpc(cls,pos);
      if(!monster) throw std::runtime_error("Cannot create monster fixture");
      monster->changeAttribute(ATR_HITPOINTS,std::min(hp,monster->attribute(ATR_HITPOINTSMAX))-
                              monster->attribute(ATR_HITPOINTS),false);
      monster->setDirection(actor->position()-pos);
      // Setup only. Keep normal collision, animation hits, HP, death and reward
      // scripts. A calm target has no routine/perceptions that can retaliate.
      if(mode=="calm") {
        monster->clearState(true);
        monster->clearAiQueue();
        monster->handle().start_aistate = 0;
        for(int i=1; i<PERC_Count; ++i) monster->setPerceptionDisable(PercType(i));
        }
      else {
        // Choose the initial opponent, then let the original combat AI decide
        // how to move/attack. This does not test autonomous target acquisition.
        monster->setTarget(actor);
        monster->setOther(actor);
        monster->startState(ScriptFn(attack),"");
        }
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
      testSaveDelayMs = 0;
      if(cmd.HasMember("delay_ms")) {
        if(!cmd["delay_ms"].IsUint() || cmd["delay_ms"].GetUint()>2000)
          throw std::runtime_error("Expected save delay in 0..2000 ms");
        testSaveDelayMs = cmd["delay_ms"].GetUint();
        }
      saveGame("save_slot_99.sav", "Automation test checkpoint");
      // Queues a save after this controller step; require committed completion.
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
