#include "mainwindow.h"
#include "commandline.h"
#include "gothic.h"
#include "network/fingerprint.h"
#include "game/playeridentity.h"
#include "world/world.h"
#include "game/gamesession.h"
#include "game/serialize.h"

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
