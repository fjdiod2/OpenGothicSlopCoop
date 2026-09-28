#include "mainwindow.h"
#include "commandline.h"
#include "gothic.h"
#include "network/fingerprint.h"
#include "game/playeridentity.h"
#include "world/world.h"

#include <Tempest/Application>
#include <Tempest/Log>

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
     Gothic::inst().checkLoading()==Gothic::LoadState::Idle) {
    if(auto world = Gothic::inst().world(); world && world->playerById(PlayerIdentity::Host)) {
      try {
        world->addPlayer(PlayerIdentity::Companion); // Reuses a live or restored actor unchanged.
        controlForPlayer(PlayerIdentity::Companion);
        if(!network.bindCompanion(status.generation))
          throw std::runtime_error("Cannot queue companion binding");
        }
      catch(const std::exception& error) {
        Tempest::Log::e("Coop binding: ",error.what());
        network.invalidate("binding_failed");
        }
      }
    }
  if(networkEvent!=status.events) {
    networkEvent = status.events;
    Tempest::Log::i("Coop: ",status.state," port=",status.port," reason=",status.reason);
    }
  }
