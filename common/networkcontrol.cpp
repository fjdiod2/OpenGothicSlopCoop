#include "mainwindow.h"
#include "commandline.h"
#include "gothic.h"
#include "network/fingerprint.h"
#include "game/playeridentity.h"
#include "world/world.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/focus.h"
#include "game/gamesession.h"
#include "game/serialize.h"
#include "utils/mouseutil.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <Tempest/MemWriter>
#include <Tempest/MemReader>
#include <filesystem>
#include <fstream>

void MainWindow::initNetwork() {
  if(CommandLine::inst().coopEndpoint().role==Coop::Role::Disabled) return;
  networkPreparation = "fingerprinting";
  try {
    Coop::FingerprintInputs inputs;
    inputs.executable = Coop::executablePath();
    inputs.data = CommandLine::inst().nestedPath({u"Data"},Tempest::Dir::FT_Dir);
    inputs.work = CommandLine::inst().nestedPath({u"_work"},Tempest::Dir::FT_Dir);
    inputs.mod = CommandLine::inst().modPath();
    inputs.mountedArchives = Resources::mountedArchives();
    auto& game = Gothic::inst();
    inputs.settings = {"game="+std::to_string(game.version().game),
                       "patch="+std::to_string(game.version().patch),
                       "language="+std::to_string(game.settingsGetI("GAME","language")),
                       "mod="+inputs.mod.filename().generic_string()};
    fingerprintJob = std::async(std::launch::async,[this,inputs] {
      return Coop::fingerprint(inputs,cancelFingerprint);
      });
    }
  catch(const std::exception& error) {
    networkPreparation = std::string("fingerprint_error: ")+error.what();
    Tempest::Log::e("Coop: ",networkPreparation);
    }
  }

bool MainWindow::canRejoinNetwork() const {
  const auto& status = network.status();
  return status.role==Coop::Role::Guest && networkPreparation.empty() && !networkLoading &&
         Gothic::inst().checkLoading()==Gothic::LoadState::Idle &&
         (status.state=="disconnected" || status.state=="failed" || status.state=="rejected");
  }

void MainWindow::rejoinNetwork() {
  if(!canRejoinNetwork()) return;
  clearInput();
  dMouse = Tempest::Point();
  remoteButtons = 0;
  networkInstalledInput = networkControlGeneration = 0;
  networkInterpolation.reset();
  networkLoadGeneration = networkLoadRevision = networkSnapshotTick = 0;
  networkSnapshotArtifact.clear();
  loadStatus = "none";
  // start() destroys every old socket, decoder, pending input/pose and ack.
  // Keep the old world read-only until a fresh verified snapshot replaces it.
  network.start(CommandLine::inst().coopEndpoint(),networkIdentity,Tempest::Application::tickCount());
  }

void MainWindow::invalidateNetworkWorld() {
  // Fence the old lifetime before any owning world pointer is discarded.
  network.invalidate("world_replaced");
  network.holdInput(false);
  clearInput();
  player.clearFocus();
  for(auto& entry:otherPlayers) entry.second->clearFocus();
  inventory.onWorldChanged();
  dialogs.onWorldChanged();
  uiKeyUp = nullptr;
  dMouse = Tempest::Point();
  remoteButtons = 0;
  networkInstalledInput = networkControlGeneration = 0;
  networkInterpolation.reset();
  networkLoadGeneration = networkLoadRevision = networkSnapshotTick = 0;
  networkSnapshotArtifact.clear();
  saveBoundary.reset();
  saveStatus = queuedSave ? "failed" : "none";
  queuedSave.reset();
  testSaveDelayMs = 0;
  worldReplacementPrepared = true;
  }

bool MainWindow::handleCoopFailure() {
  auto& game=Gothic::inst();
  if(game.checkLoading()!=Gothic::LoadState::Idle || !game.gameSession() ||
     !game.gameSession()->observeCoopFailure()) return false;
  if(!handledCoopFailure) {
    handledCoopFailure=true;
    clearInput();
    testHeldKeys.clear();
    testReleaseAt=0;
    dMouse=Tempest::Point();
    inventory.onWorldChanged();
    dialogs.onWorldChanged();
    uiKeyUp=nullptr;
    if(queuedSave) {
      queuedSave.reset();
      saveStatus="denied_failure";
      testSaveDelayMs=0;
      }
    update();
    }
  return true;
  }

void MainWindow::tickNetwork() {
  if(fingerprintJob.valid() && fingerprintJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
    try {
      networkIdentity = fingerprintJob.get();
      networkPreparation.clear();
      network.start(CommandLine::inst().coopEndpoint(),networkIdentity,Tempest::Application::tickCount());
      }
    catch(const std::exception& error) {
      networkPreparation = std::string("fingerprint_error: ")+error.what();
      Tempest::Log::e("Coop: ",networkPreparation);
      }
    }
  network.tick(Tempest::Application::tickCount());
  const auto& status = network.status();
  // The loader temporarily owns the entire game during saves as well as loads.
  // Resolve by persistent ID on the game thread; Session never retains NPC pointers.
  if(status.role==Coop::Role::Host && status.state=="connected" && status.boundPlayer==0 &&
     Gothic::inst().checkLoading()==Gothic::LoadState::Idle &&
     !dialogs.isActive() && !inventory.isActive() && !video.isActive()) {
    if(auto world = Gothic::inst().world(); world && world->playerById(PlayerIdentity::Host)) {
      try {
        if(world->currentCs()!=nullptr) return;
        world->addPlayer(PlayerIdentity::Companion); // Reuses a live or restored actor unchanged.
        controlForPlayer(PlayerIdentity::Companion);
        if(!network.bindCompanion(status.generation))
          throw std::runtime_error("Cannot queue companion binding");
        clearInput();
        Coop::Bytes snapshot;
        {
        Tempest::MemWriter output(snapshot);
        Serialize archive(output);
        Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
        std::fill_n(static_cast<uint8_t*>(preview.data()),4,uint8_t(0));
        Gothic::inst().gameSession()->save(archive,"Co-op snapshot",preview);
        archive.finish();
        }
        networkSnapshotTick = world->tickCount();
        networkSnapshotArtifact.clear();
        if(!testControlDir.empty()) {
          networkSnapshotArtifact = (std::filesystem::path(testControlDir)/
                                     ("snapshot-"+std::to_string(status.generation)+".zip")).string();
          std::ofstream output(networkSnapshotArtifact,std::ios::binary);
          output.write(reinterpret_cast<const char*>(snapshot.data()),std::streamsize(snapshot.size()));
          if(!output) throw std::runtime_error("Cannot record test snapshot");
          }
        if(!network.sendSnapshot(status.generation,1,std::move(snapshot)))
          throw std::runtime_error("Cannot start snapshot transfer");
        }
      catch(const std::exception& error) {
        Tempest::Log::e("Coop binding: ",error.what());
        network.invalidate("binding_failed");
        }
      }
    }
  auto& game = Gothic::inst();
  if(game.checkLoading()==Gothic::LoadState::Idle && game.world())
    game.world()->trackNetworkPickups(status.role==Coop::Role::Host && status.snapshotState=="ready");
  game.setNetworkPaused(status.role==Coop::Role::Host && status.boundPlayer!=0 && status.snapshotState!="ready");
  tickNetworkInput();
  if(status.role==Coop::Role::Guest && !networkLoading && game.checkLoading()==Gothic::LoadState::Idle) {
    if(auto snapshot = network.takeSnapshot()) {
      networkLoading = true;
      networkLoadGeneration = snapshot->generation;
      networkLoadRevision = snapshot->revision;
      loadStatus = "pending";
      setGameImpl(nullptr);
      game.startLoad("LOADING.TGA",[bytes=std::move(snapshot->data)](std::unique_ptr<GameSession>&& old) {
        old.reset();
        Tempest::MemReader input(bytes);
        Serialize archive(input);
        auto next = std::make_unique<GameSession>(archive,GameSession::Authority::Guest);
        if(!next->world()->playerById(PlayerIdentity::Host) || !next->world()->playerById(PlayerIdentity::Companion))
          throw std::runtime_error("Snapshot does not contain both players");
        return next;
        });
      rootMenu.closeAll();
      }
    }
  if(networkEvent!=status.events) {
    networkEvent = status.events;
    Tempest::Log::i("Coop: ",status.state," port=",status.port," reason=",status.reason);
    }
  }

void MainWindow::finishNetworkLoad(bool success) {
  if(!networkLoading) return;
  networkLoading = false;
  auto& game = Gothic::inst();
  const auto& status = network.status();
  if(success && game.world() && status.generation==networkLoadGeneration) {
    try {
      selectLocalPlayer(PlayerIdentity::Companion);
      if(network.snapshotLoaded(networkLoadGeneration,networkLoadRevision)) {
        networkSnapshotTick = game.world()->tickCount();
        return;
        }
      }
    catch(const std::exception& error) {
      Tempest::Log::e("Coop snapshot activation: ",error.what());
      }
    }
  // The loader may finish after the connection was cancelled. Never render that
  // session or acknowledge it: disposal occurs before this frame is drawn.
  setGameImpl(nullptr);
  onWorldLoaded();
  loadStatus = "failed";
  network.invalidate("snapshot_load_failed");
  }

namespace {
constexpr std::pair<KeyCodec::Action,uint32_t> networkActions[] = {
  {KeyCodec::ActionGeneric,Coop::Attack}, {KeyCodec::WeaponMele,Coop::Melee},
  {KeyCodec::Forward,Coop::Forward}, {KeyCodec::Back,Coop::Backward},
  {KeyCodec::RotateL,Coop::TurnLeft}, {KeyCodec::RotateR,Coop::TurnRight},
  {KeyCodec::Left,Coop::StrafeLeft}, {KeyCodec::Right,Coop::StrafeRight}
  };
}

void MainWindow::networkKey(Tempest::KeyEvent& event, bool down) {
  auto& game = Gothic::inst();
  if(game.isCoopFailed() || network.status().snapshotState!="ready" || game.checkLoading()!=Gothic::LoadState::Idle ||
     game.localPlayerId()!=PlayerIdentity::Companion) return;
  const auto mapped = [&](const Tempest::KeyEvent& e) { auto a=keycodec.tr(e); return a==KeyCodec::Weapon ? KeyCodec::WeaponMele : a; };
  const auto action = mapped(event);
  if(std::none_of(std::begin(networkActions),std::end(networkActions),[&](const auto& a) { return a.first==action; })) return;
  const bool pressed = down && networkKeys.find(event.key)==networkKeys.end();
  if(down) networkKeys.insert(event.key); else networkKeys.erase(event.key);
  uint32_t buttons = 0;
  bool interacting = false;
  for(auto key:networkKeys) {
    const Tempest::KeyEvent held(key);
    interacting |= mapped(held)==KeyCodec::ActionGeneric;
    for(const auto& [a,bit]:networkActions)
      if(mapped(held)==a) buttons |= bit;
    }
  const auto ws=game.localPlayer()->weaponState();
  const bool melee=ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H;
  if(!melee) {
    buttons &= ~Coop::Attack;
    if(interacting) buttons &= Coop::Melee; // Unarmed action+forward is pickup.
    }
  Item* item = nullptr;
  if(pressed && action==KeyCodec::ActionGeneric && ws==WeaponState::NoWeapon && game.world())
    item = game.world()->findFocus(*game.localPlayer(),Focus()).item;
  if(buttons!=networkButtons || item) {
    networkButtons = buttons;
    sendNetworkInput(0,0,item ? game.world()->objectRegistry().worldNamespace() : 0,
                         item ? game.world()->objectId(item) : 0);
    }
  event.accept();
  }

void MainWindow::sendNetworkInput(int x, int y, uint64_t pickupWorld, uint64_t pickupItem) {
  if(Gothic::inst().isCoopFailed() || network.status().role!=Coop::Role::Guest || network.status().snapshotState!="ready") return;
  const auto clamp = [](int value) { return int16_t(std::clamp(value,-int(Coop::MaxLookDelta),int(Coop::MaxLookDelta))); };
  if(!network.sendInput({networkButtons,clamp(x),clamp(y),pickupWorld,pickupItem}))
    network.invalidate("input_queue_full");
  }

void MainWindow::sendNetworkEquipment(size_t instance, bool equipped) {
  auto& game = Gothic::inst();
  if(game.isCoopFailed() || network.status().role!=Coop::Role::Guest || network.status().snapshotState!="ready" ||
     game.checkLoading()!=Gothic::LoadState::Idle || instance>UINT32_MAX ||
     !game.localPlayer() || inventory.activePlayer()!=game.localPlayer()) return;
  if(!network.sendInput({networkButtons,0,0,0,0,uint64_t(instance)+1,uint8_t(equipped ? 1 : 2)}))
    network.invalidate("input_queue_full");
  }

void MainWindow::sendNetworkConsumable(size_t instance) {
  auto& game=Gothic::inst();
  if(game.isCoopFailed() || network.status().role!=Coop::Role::Guest || network.status().snapshotState!="ready" ||
     game.checkLoading()!=Gothic::LoadState::Idle || instance>UINT32_MAX ||
     !game.localPlayer() || inventory.activePlayer()!=game.localPlayer()) return;
  if(!network.sendInput({networkButtons,0,0,0,0,uint64_t(instance)+1,3}))
    network.invalidate("input_queue_full");
  }

void MainWindow::tickNetworkInput(bool apply) {
  auto& game = Gothic::inst();
  const auto& status = network.status();
  if(game.checkLoading()!=Gothic::LoadState::Idle) return;
  // A connection alone grants no control. Revoke its lease before the next
  // simulation step as soon as its binding or snapshot readiness is lost.
  // During asynchronous save/load ownership this waits until the game thread
  // owns the world again; tick() cannot simulate it in the meantime.
  const auto controlGeneration = status.boundPlayer==PlayerIdentity::Companion &&
                                 status.snapshotState=="ready" ? status.generation : 0;
  if(networkControlGeneration!=controlGeneration) {
    networkKeys.clear();
    networkButtons = remoteButtons = 0;
    networkInstalledInput = 0;
    networkPickupRequests = networkPickupAccepted = 0;
    networkEquipmentRequests = networkEquipmentAccepted = 0;
    networkConsumableRequests = networkConsumableAccepted = 0;
    networkEquipmentSent = {};
    networkProgressSent = {};
    networkPersonalSent = {};
    networkCombatSent.clear();
    networkCombatCursor = 0;
    if(status.role==Coop::Role::Host && game.world())
      for(uint32_t i=0;i<game.world()->npcCount();++i) {
        auto npc=game.world()->npcById(i);
        if(!npc->isPlayer()) networkCombatSent.emplace(game.world()->objectId(npc),Coop::CombatNpc{npc->networkPose(),npc->networkCombatState()});
        }
    networkInterpolation.reset();
    if(status.role==Coop::Role::Host && game.world() && game.world()->playerById(PlayerIdentity::Companion))
      controlForPlayer(PlayerIdentity::Companion).clearInput();
    networkControlGeneration = controlGeneration;
    }
  if(game.isCoopFailed()) {
    // Discard late commands without applying or acknowledging them. A later
    // load fences this connection and its sequence space with a new generation.
    while(network.takeInput()) ++discardedFailureInputs;
    return;
    }
  if(!apply || status.role!=Coop::Role::Host || status.snapshotState!="ready" || !game.world()) return;
  auto& control = controlForPlayer(PlayerIdentity::Companion);
  float lookX = 0, lookY = 0;
  while(auto message = network.takeInput()) {
    auto input = std::get<Coop::Input>(message->payload);
    const auto ws=game.world()->playerById(PlayerIdentity::Companion)->weaponState();
    if(ws!=WeaponState::Fist && ws!=WeaponState::W1H && ws!=WeaponState::W2H) input.buttons &= ~Coop::Attack;
    for(const auto& [action,bit]:networkActions) {
      if((input.buttons & bit)==(remoteButtons & bit)) continue;
      if(input.buttons & bit)
        control.onKeyPressed(action,Tempest::Event::K_NoKey,KeyCodec::Mapping::Primary);
      else
        control.onKeyReleased(action,KeyCodec::Mapping::Primary);
      }
    remoteButtons = input.buttons;
    const float sensitivity = game.settingsGetF("GAME","mouseSensitivity")/MouseUtil::mouseSysSpeed();
    const float x = -270.f*float(input.lookX)*sensitivity/float(std::max(w(),1));
    float y = -270.f*float(input.lookY)*sensitivity/float(std::max(h(),1));
    if(game.settingsGetI("GAME","camLookaroundInverse")) y = -y;
    lookX += x;
    lookY += y;
    networkInstalledInput = message->sequence;
    if(input.pickupItem!=0) {
      ++networkPickupRequests;
      if(control.pickupObject(input.pickupWorld,input.pickupItem)) ++networkPickupAccepted;
      }
    if(input.equipmentItem!=0 && input.equipmentAction==3) {
      ++networkConsumableRequests;
      if(!dialogs.isActive() && !video.isActive() && game.world()->currentCs()==nullptr &&
         game.world()->playerById(PlayerIdentity::Companion)->requestConsumable(size_t(input.equipmentItem-1)))
        ++networkConsumableAccepted;
      }
    else if(input.equipmentItem!=0) {
      ++networkEquipmentRequests;
      if(!dialogs.isActive() && !video.isActive() && game.world()->currentCs()==nullptr &&
         game.world()->playerById(PlayerIdentity::Companion)->requestWeaponEquipment(size_t(input.equipmentItem-1),input.equipmentAction==1))
        ++networkEquipmentAccepted;
      }
    }
  // Bound the accumulated impulse per frame, including a burst of packets.
  // The sender never supplies elapsed simulation time.
  control.onRotateMouse(std::clamp(lookX,-36.f,36.f),std::clamp(lookY,-36.f,36.f));
  }

void MainWindow::updateNetworkPoses() {
  auto& game = Gothic::inst();
  const auto& status = network.status();
  auto world = game.world();
  if(game.checkLoading()!=Gothic::LoadState::Idle || !world || status.snapshotState!="ready") return;
  try {
    if(status.role==Coop::Role::Host) {
      const std::array<Coop::WeaponEquipment,2> equipment = {world->playerById(1)->networkEquipment(),world->playerById(2)->networkEquipment()};
      const std::array<Coop::CombatState,2> progression = {world->playerById(1)->networkCombatState(),world->playerById(2)->networkCombatState()};
      const std::array<Coop::PlayerState,2> personal = {world->playerById(1)->networkPlayerState(),world->playerById(2)->networkPlayerState()};
      std::vector<Coop::CombatNpc> npcs;
      // Fair bounded batches. Remember changes only when queued; do not lose a
      // death because many other NPCs changed in the same frame.
      for(size_t examined=0;examined<world->npcCount() && npcs.size()<Coop::MaxCombatNpcs;++examined) {
        networkCombatCursor %= world->npcCount();
        auto npc=world->npcById(uint32_t(networkCombatCursor++));
        if(npc->isPlayer()) continue;
        const auto state=npc->networkCombatState();
        const auto previous=networkCombatSent.find(world->objectId(npc));
        const bool nearby=(npc->position()-world->playerById(1)->position()).quadLength()<3000.f*3000.f ||
                          (npc->position()-world->playerById(2)->position()).quadLength()<3000.f*3000.f;
        if(!nearby && previous!=networkCombatSent.end() && previous->second.state==state) continue;
        Coop::CombatNpc next{npc->networkPose(),state};
        if(previous==networkCombatSent.end() || previous->second!=next) npcs.push_back(std::move(next));
        }
      if(status.posesSent!=0 && status.poseTick==world->tickCount() && status.inputApplied==networkInstalledInput &&
         !world->hasNetworkPickups() && equipment==networkEquipmentSent && progression==networkProgressSent && personal==networkPersonalSent && npcs.empty()) return;
      const auto revision = std::max(status.snapshotRevision,status.posesSent)+1;
      if(revision==0) throw std::runtime_error("Pose revision exhausted");
      while(status.inputApplied<networkInstalledInput)
        if(!network.inputApplied(status.generation,status.inputApplied+1,revision))
          throw std::runtime_error("Cannot acknowledge applied input");
      Coop::Poses poses;
      poses.world = world->objectRegistry().worldNamespace();
      poses.tick = world->tickCount();
      poses.appliedInput = status.inputApplied;
      for(size_t i=0; i<poses.actors.size(); ++i)
        poses.actors[i] = world->playerById(i+1)->networkPose();
      poses.pickups = world->takeNetworkPickups();
      poses.equipment = equipment;
      poses.progression = progression;
      poses.personal = personal;
      poses.npcs = std::move(npcs);
      if(!network.sendPoses(status.generation,revision,poses))
        throw std::runtime_error("Cannot queue authoritative poses");
      networkEquipmentSent = equipment;
      networkProgressSent = progression;
      networkPersonalSent = personal;
      for(const auto& npc:poses.npcs) networkCombatSent[npc.pose.object]=npc;
      return;
      }
    // Advance the existing blend before retargeting it. Otherwise a steady
    // stream would restart at alpha zero every frame and never move.
    if(!networkInterpolation.empty()) {
      const auto frame = networkInterpolation.sample(Tempest::Application::tickCount());
      const auto& poses = std::get<Coop::Poses>(frame.payload);
      for(size_t i=0; i<2; ++i) world->playerById(i+1)->applyNetworkPose(poses.actors[i]);
      // An acknowledgement means both rendered transforms reached that exact
      // authoritative sample, not merely that its packet arrived.
      if(networkInterpolation.alpha()==1.f && frame.revision>status.posesApplied &&
         !network.posesApplied(frame.generation,frame.revision))
        throw std::runtime_error("Cannot acknowledge interpolated poses");
      }
    while(auto message = network.takePoses()) {
      const auto& poses = std::get<Coop::Poses>(message->payload);
      if(poses.world!=world->objectRegistry().worldNamespace())
        throw std::runtime_error("Pose belongs to another world");
      std::array<Npc*,2> actors = {};
      for(size_t i=0; i<actors.size(); ++i) {
        actors[i] = world->npcByObjectId(poses.world,poses.actors[i].object);
        if(!actors[i] || !actors[i]->validateNetworkPose(poses.actors[i]) || !actors[i]->validateNetworkCombatState(poses.progression[i]))
          throw std::runtime_error("Unknown pose identity or animation");
        }
      for(const auto& npc:poses.npcs) {
        auto actor=world->npcByObjectId(poses.world,npc.pose.object);
        if(!actor || actor->isPlayer() || !actor->validateNetworkPose(npc.pose) || !actor->validateNetworkCombatState(npc.state))
          throw std::runtime_error("Unknown combat NPC, reference or animation");
        }
      if(!world->applyNetworkInventory(poses.pickups,poses.equipment,poses.personal))
        throw std::runtime_error("Inventory update does not match the snapshot state");
      for(size_t i=0;i<actors.size();++i) {
        actors[i]->applyNetworkCombatState(poses.progression[i]);
        actors[i]->applyNetworkPlayerState(poses.personal[i]);
        }
      for(const auto& npc:poses.npcs) {
        auto actor=world->npcByObjectId(poses.world,npc.pose.object);
        actor->applyNetworkCombatState(npc.state);
        actor->applyNetworkPose(npc.pose);
        }
      if(!poses.pickups.empty()) {
        player.clearFocus();
        for(auto& entry:otherPlayers) entry.second->clearFocus();
        }
      // Validate both actors before changing either presentation replica.
      const std::array<Coop::ActorPose,2> displayed = {actors[0]->networkPose(),actors[1]->networkPose()};
      networkInterpolation.push(*message,displayed,Tempest::Application::tickCount());
      game.gameSession()->syncPresentationTick(poses.tick);
      }

    }
  catch(const std::exception& error) {
    Tempest::Log::e("Coop poses: ",error.what());
    network.invalidate("pose_apply_failed");
    }
  }
