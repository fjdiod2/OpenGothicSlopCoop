#pragma once
#include "game/checkpoint.h"

#include "camera.h"
#include "resources.h"

#include <Tempest/Window>
#include <Tempest/CommandBuffer>
#include <Tempest/Fence>
#include <Tempest/VulkanApi>
#include <Tempest/Device>
#include <Tempest/VertexBuffer>
#include <Tempest/UniformBuffer>
#include <Tempest/VectorImage>
#include <Tempest/Event>
#include <Tempest/Pixmap>
#include <Tempest/Sprite>
#include <Tempest/Font>
#include <Tempest/TextureAtlas>
#include <Tempest/Timer>
#include <Tempest/Swapchain>

#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <set>
#include "network/session.h"
#include "network/interpolation.h"

#include "world/world.h"
#include "world/focus.h"
#include "game/playercontrol.h"
#include "graphics/renderer.h"
#include "ui/dialogmenu.h"
#include "ui/inventorymenu.h"
#include "ui/chapterscreen.h"
#include "ui/documentmenu.h"
#include "ui/videowidget.h"
#include "ui/menuroot.h"
#include "ui/consolewidget.h"
#include "ui/touchinput.h"

#include "utils/keycodec.h"
#include "resources.h"

class MenuRoot;
class GameSession;
class Interactive;

class MainWindow : public Tempest::Window {
  public:
    explicit MainWindow(Tempest::Device& device);
    ~MainWindow() override;

    float uiScale() const;

  private:
    void paintEvent     (Tempest::PaintEvent& event) override;
    void resizeEvent    (Tempest::SizeEvent & event) override;

    void mouseDownEvent (Tempest::MouseEvent& event) override;
    void mouseUpEvent   (Tempest::MouseEvent& event) override;
    void mouseDragEvent (Tempest::MouseEvent& event) override;
    void mouseMoveEvent (Tempest::MouseEvent& event) override;
    void mouseWheelEvent(Tempest::MouseEvent& event) override;

    void keyDownEvent   (Tempest::KeyEvent&   event) override;
    void keyRepeatEvent (Tempest::KeyEvent&   event) override;
    void keyUpEvent     (Tempest::KeyEvent&   event) override;

    void focusEvent     (Tempest::FocusEvent&  event) override;

    void paintFocus     (Tempest::Painter& p, const Focus& fc, const Tempest::Matrix4x4& vp);
    void paintFocus     (Tempest::Painter& p, Tempest::Rect rect);

    void drawBar(Tempest::Painter& p, const Tempest::Texture2d *bar, int x, int y, float v, Tempest::AlignFlag flg);
    void drawMsg(Tempest::Painter& p);
    void drawProgress(Tempest::Painter& p, int x, int y, int w, int h, float v);
    void drawLoading (Tempest::Painter& p,int x,int y,int w,int h);
    void drawSaving  (Tempest::Painter& p);
    void drawSaving  (Tempest::Painter& p, const Tempest::Texture2d& back, int w, int h, float scale);

    void startGame(std::string_view slot);
    void loadGame (std::string_view slot);
    void saveGame (std::string_view slot, std::string_view name);

    void onVideo(std::string_view fname);
    void onStartLoading();
    void onWorldLoaded();
    void onSessionExit();
    void onBenchmarkFinished();
    void setGameImpl(std::unique_ptr<GameSession>&& w);
    void clearInput();
    void selectLocalPlayer(uint64_t id);
    void rebuildPlayerControls();
    void setFullscreen(bool fs);

    void processMouse(Tempest::MouseEvent& event, bool enable);
    void tickMouse(uint64_t dt);
    void onSettings();

    void setupUi();

    void render() override;

    void initNetwork();
    void tickNetwork();
    bool canRejoinNetwork() const;
    void rejoinNetwork();
    void invalidateNetworkWorld();
    bool worldReplacementPrepared = false;
    Coop::Session network;
    Coop::Hello networkIdentity;
    std::atomic_bool cancelFingerprint = false;
    std::future<Coop::Hello> fingerprintJob;
    std::string networkPreparation;
    uint64_t networkEvent = 0;
    bool networkLoading = false;
    uint64_t networkLoadGeneration = 0, networkLoadRevision = 0, networkSnapshotTick = 0;
    std::string networkSnapshotArtifact;
    void finishNetworkLoad(bool success);
    void networkKey(Tempest::KeyEvent& event, bool down);
    void sendNetworkInput(int x=0, int y=0, uint64_t pickupWorld=0, uint64_t pickupItem=0);
    void tickNetworkInput(bool apply = false);
    void updateNetworkPoses();
    std::set<Tempest::Event::KeyType> networkKeys;
    uint32_t networkButtons = 0, remoteButtons = 0;
    uint64_t networkControlGeneration = 0;
    uint64_t networkInstalledInput = 0;
    uint64_t networkPickupRequests = 0, networkPickupAccepted = 0;
    Coop::PoseInterpolation networkInterpolation;

    // Local, opt-in development interface; all commands run on the game thread.
    void initTestControl();
    void pollTestControl();
    void replyTestControl(std::string_view id, std::string_view error = {},
                          std::string_view artifact = {});
    std::string testControlDir;
    std::string testSessionId;
    std::string saveStatus = "none";
    struct SaveRequest { std::string slot, name; uint32_t delayMs = 0; };
    std::optional<SaveRequest> queuedSave;
    std::optional<CheckpointBoundary> saveBoundary;
    bool returningFromSave = false;
    uint32_t testSaveDelayMs = 0;
    void beginQueuedSave();
    std::string loadStatus = "none";
    struct HudObservation {
      uint64_t player = 0;
      int hp = 0, hpMax = 0, mana = 0, manaMax = 0;
      } paintedHud;
    std::string testRequestId;
    std::vector<Tempest::Event::KeyType> testHeldKeys;
    uint64_t testInputPlayer = 1;
    uint64_t testHeldPlayer = 1;
    bool testAsyncInput = false;
    PlayerControl& controlForPlayer(uint64_t id);
    void dispatchPlayerKey(uint64_t id, Tempest::KeyEvent& event, bool down);
    uint64_t testReleaseAt = 0;
    uint64_t testPollAt = 0;

    uint64_t tick();
    void     updateAnimation(uint64_t dt);
    void     tickCamera(uint64_t dt);
    void     isDialogClosed(bool& ret);

    template<Tempest::KeyEvent::KeyType k>
    void     onMarvinKey();

    Camera::Mode solveCameraMode() const;

    enum RuntimeMode : uint8_t {
      R_Normal,
      R_Suspended,
      R_Step,
      };

    Tempest::Device&      device;
    Tempest::Swapchain    swapchain;
    Tempest::TextureAtlas atlas;
    Shaders               shaders;
    Renderer              renderer;

    Tempest::VectorImage  uiLayer, numOverlay;
    Tempest::VectorImage::Mesh uiMesh [Resources::MaxFramesInFlight];
    Tempest::VectorImage::Mesh numMesh[Resources::MaxFramesInFlight];

    Tempest::Fence         fence   [Resources::MaxFramesInFlight];
    Tempest::CommandBuffer commands[Resources::MaxFramesInFlight];
    uint8_t                cmdId = 0;
    uint64_t               renderedFrames = 0;

    Tempest::Texture2d        background;
    const Tempest::Texture2d* loadBox=nullptr;
    const Tempest::Texture2d* loadVal=nullptr;

    const Tempest::Texture2d* barBack=nullptr;
    const Tempest::Texture2d* barHp  =nullptr;
    const Tempest::Texture2d* barMisc=nullptr;
    const Tempest::Texture2d* barMana=nullptr;

    const Tempest::Texture2d* focusImg=nullptr;

    const Tempest::Texture2d* saveback=nullptr;

    bool                      mouseP[Tempest::MouseEvent::ButtonBack]={};

    KeyCodec                  keycodec;

    MenuRoot                  rootMenu;
    VideoWidget               video;
    InventoryMenu             inventory;
    DialogMenu                dialogs;
    DocumentMenu              document;
    ChapterScreen             chapter;
    ConsoleWidget             console;
#if defined(__MOBILE_PLATFORM__)
    TouchInput                mobileUi;
#endif
    RuntimeMode               runtimeMode = R_Normal;

    Tempest::Widget*          uiKeyUp=nullptr;
    Tempest::Point            dMouse;
    PlayerControl             player;
    std::map<uint64_t, std::unique_ptr<PlayerControl>> otherPlayers;
    uint64_t                  lastTick=0;

    Tempest::Shortcut         funcKey[11];
    Tempest::Shortcut         displayPos;

    struct BenchmarkData {
      std::vector<uint64_t> low1procent;
      size_t                numFrames = 0;
      double                fpsSum = 0;
      void                  push(uint64_t t);
      void                  clear();
      };
    struct Fps {
      uint64_t dt[10]={};
      double   get() const;
      void     push(uint64_t t);
      };
    Fps           fps;
    BenchmarkData benchmark;
    uint64_t      maxFpsInv = 0;
  };
