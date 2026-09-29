#pragma once

#include <Tempest/Font>
#include <Tempest/Texture2d>
#include <Tempest/Timer>
#include <Tempest/Widget>
#include <Tempest/SoundEffect>
#include <Tempest/SoundDevice>

#include "game/gamescript.h"
#include "camera.h"
#include "network/protocol.h"

class Npc;
class Interactive;
class InventoryMenu;
class GthFont;

class DialogMenu : public Tempest::Widget {
  public:
    DialogMenu(InventoryMenu& trade);
    ~DialogMenu();

    void tick(uint64_t dt);
    void clear();

    void onWorldChanged();
    void saveNetwork(Serialize& out) const;
    void applyNetwork(Serialize& in);
    bool takeTradeInventoryUpdate();
    uint64_t owner() const { return conversationOwner; }
    uint64_t conversation() const { return conversationId; }
    uint64_t revision() const { return conversationRevision; }
    bool beginGuest(Npc& npc);
    void remoteInput(const Coop::Input& input);
    void cancelGuest();
    std::function<void(const Coop::Input&)> sendInput;

    bool isMobsiDialog() const;
    void dialogCamera(Camera& camera);

    void openPipe(Npc &player, Npc& npc, AiOuputPipe*& out);
    bool isNpcInDialog(const Npc* npc) const;

    bool isActive() const;
    bool hasContent() const;

    // Observe the existing menu without evaluating conditions or executing scripts.
    struct Observation {
      bool choicesActive;
      size_t selection;
      const Npc* player;
      const Npc* npc;
      std::string_view text;
      const std::vector<GameScript::DlgChoice>& choices;
      };
    Observation observation() const {
      return {isChoiceMenuActive(),dlgSel,pl,other,current.time>0 ? current.txt : std::string_view(),choice};
      }

    void print      (std::string_view msg);
    void printScreen(std::string_view msg, int x, int y, int time, const GthFont &font);
    void drawMsg    (Tempest::Painter& p, int offsetY);

    void keyDownEvent   (Tempest::KeyEvent&   event) override;
    void keyUpEvent     (Tempest::KeyEvent&   event) override;

  protected:
    void paintEvent (Tempest::PaintEvent& e) override;
    void paintChoice(Tempest::PaintEvent& e);

    void mouseDownEvent (Tempest::MouseEvent& event) override;
    void mouseWheelEvent(Tempest::MouseEvent& event) override;

    void onSelect();
    void setupSettings();
    bool isChoiceMenuActive() const;

  private:
    const Tempest::Texture2d* tex    =nullptr;
    const Tempest::Texture2d* ambient=nullptr;

    struct Pipe : AiOuputPipe {
      Pipe(DialogMenu& owner):owner(owner){}

      bool output   (Npc& npc, std::string_view text) override;
      bool outputSvm(Npc& npc, std::string_view text) override;
      bool outputOv (Npc& npc, std::string_view text) override;
      bool printScr (Npc& npc, int time, std::string_view msg, int x, int y, std::string_view font) override;

      bool close() override;
      bool isFinished() override;

      DialogMenu& owner;
      };

    enum {
      MAX_PRINT = 3,
      ANIM_TIME = 200,
      };

    enum class State : uint8_t {
      Idle    =0,
      PreStart=1,
      Active  =2
      };

    struct Entry {
      std::string txt;
      uint64_t    msgTime = 0;
      uint64_t    time    = 0;
      };

    struct Forward {
      std::string txt;
      Npc*        npc=nullptr;
      };

    struct PScreen {
      std::string    txt;
      const GthFont* font=nullptr;
      uint64_t       time=0;
      int            x=-1;
      int            y=-1;
      };

    bool onStart(Npc& pl,Npc& other);
    bool guestChoiceAllowed(const GameScript::DlgChoice& entry) const;
    bool trainingChoice(const GameScript::DlgChoice& entry) const;
    void sendRemote(uint8_t action, uint64_t target=0);
    bool localOwner() const;
    void onEntry(const GameScript::DlgChoice& e);
    void onDoneText();
    void close();
    bool aiOutput  (Npc& npc, std::string_view msg);
    bool aiPrintScr(Npc& npc, int time, std::string_view msg, int x, int y, std::string_view font);
    bool aiClose();

    bool haveToWaitOutput() const;
    bool hasPrintMsg() const;
    bool haveToShowSubtitles(bool isPl) const;

    void drawTextMultiline(Tempest::Painter& p, int x, int y, int w, int h, std::string_view txt, bool isPl);
    Tempest::Size processTextMultiline(Tempest::Painter* p, int x, int y, int w, int h, std::string_view txt, bool isPl);

    void startTrade();
    void tradeRequest(uint8_t action, uint64_t target=0);
    void skipPhrase();

    InventoryMenu&                      trade;
    Pipe                                pipe;

    Tempest::SoundDevice                soundDevice;

    std::vector<GameScript::DlgChoice>  choice;
    GameScript::DlgChoice               selected;
    Npc*                                pl   =nullptr;
    Npc*                                other=nullptr;
    size_t                              dlgSel=0;
    uint32_t                            depth=0;
    std::vector<uint32_t>               except;

    bool                                replica=false, replicaChoices=false;
    uint64_t                            conversationOwner=0, conversationId=0, conversationRevision=0;
    Npc*                                stagedPlayer=nullptr;
    bool                                tradeInventoryDirty=false;
    bool                                executingTraining=false;
    int32_t                             trainingBowBefore=0;
    std::string                         trainingMessage;
    State                               state=State::Idle;
    Entry                               current;
    Tempest::SoundEffect                currentSnd;
    bool                                curentIsPl=false;
    bool                                dlgTrade=false;
    int32_t                             mobsiState=-1;

    std::vector<PScreen>                pscreen;
    PScreen                             printMsg[MAX_PRINT];
    uint64_t                            remPrint=0;

    bool                                dlgAnimation        = true;
    bool                                showSubtitles       = true;
    bool                                showSubtitlesPlayer = true;
    uint64_t                            choiceAnimTime      = 0;
  };
