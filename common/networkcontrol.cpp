#include "mainwindow.h"
#include "commandline.h"
#include "gothic.h"
#include "network/fingerprint.h"
#include "game/playeridentity.h"
#include "world/world.h"
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
constexpr KeyCodec::Action networkActions[] = {
  KeyCodec::Forward, KeyCodec::Back, KeyCodec::RotateL,
  KeyCodec::RotateR, KeyCodec::Left, KeyCodec::Right
  };
}

void MainWindow::networkKey(Tempest::KeyEvent& event, bool down) {
  auto& game = Gothic::inst();
  if(network.status().snapshotState!="ready" || game.checkLoading()!=Gothic::LoadState::Idle ||
     game.localPlayerId()!=PlayerIdentity::Companion) return;
  const auto action = keycodec.tr(event);
  if(std::find(std::begin(networkActions),std::end(networkActions),action)==std::end(networkActions)) return;
  if(down) networkKeys.insert(event.key); else networkKeys.erase(event.key);
  uint32_t buttons = 0;
  for(auto key:networkKeys) {
    const Tempest::KeyEvent held(key);
    for(size_t i=0; i<std::size(networkActions); ++i)
      if(keycodec.tr(held)==networkActions[i]) buttons |= uint32_t(1)<<i;
    }
  if(buttons!=networkButtons) {
    networkButtons = buttons;
    sendNetworkInput();
    }
  event.accept();
  }

void MainWindow::sendNetworkInput(int x, int y) {
  if(network.status().role!=Coop::Role::Guest || network.status().snapshotState!="ready") return;
  const auto clamp = [](int value) { return int16_t(std::clamp(value,-int(Coop::MaxLookDelta),int(Coop::MaxLookDelta))); };
  if(!network.sendInput({networkButtons,clamp(x),clamp(y)}))
    network.invalidate("input_queue_full");
  }

void MainWindow::tickNetworkInput() {
  auto& game = Gothic::inst();
  const auto& status = network.status();
  if(game.checkLoading()!=Gothic::LoadState::Idle) return;
  if(networkControlGeneration!=status.generation) {
    networkKeys.clear();
    networkButtons = remoteButtons = 0;
    if(status.role==Coop::Role::Host && game.world() && game.world()->playerById(PlayerIdentity::Companion))
      controlForPlayer(PlayerIdentity::Companion).clearInput();
    networkControlGeneration = status.generation;
    }
  if(status.role!=Coop::Role::Host || status.snapshotState!="ready" || !game.world()) return;
  auto& control = controlForPlayer(PlayerIdentity::Companion);
  float lookX = 0, lookY = 0;
  while(auto message = network.takeInput()) {
    const auto input = std::get<Coop::Input>(message->payload);
    for(size_t i=0; i<std::size(networkActions); ++i) {
      const auto bit = uint32_t(1)<<i;
      if((input.buttons & bit)==(remoteButtons & bit)) continue;
      if(input.buttons & bit)
        control.onKeyPressed(networkActions[i],Tempest::Event::K_NoKey,KeyCodec::Mapping::Primary);
      else
        control.onKeyReleased(networkActions[i],KeyCodec::Mapping::Primary);
      }
    remoteButtons = input.buttons;
    const float sensitivity = game.settingsGetF("GAME","mouseSensitivity")/MouseUtil::mouseSysSpeed();
    const float x = -270.f*float(input.lookX)*sensitivity/float(std::max(w(),1));
    float y = -270.f*float(input.lookY)*sensitivity/float(std::max(h(),1));
    if(game.settingsGetI("GAME","camLookaroundInverse")) y = -y;
    lookX += x;
    lookY += y;
    if(!network.inputApplied(message->generation,message->sequence,status.snapshotRevision)) {
      control.clearInput();
      remoteButtons = 0;
      network.invalidate("input_ack_failed");
      return;
      }
    }
  // Bound the accumulated impulse per frame, including a burst of packets.
  // The sender never supplies elapsed simulation time.
  control.onRotateMouse(std::clamp(lookX,-36.f,36.f),std::clamp(lookY,-36.f,36.f));
  }
