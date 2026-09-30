#pragma once
#include <Tempest/Widget>
#include <functional>
#include <cstdint>

class EmoteMenu final : public Tempest::Widget {
  public:
    EmoteMenu();
    void open();
    void close();
    bool isActive() const { return active; }
    std::function<void(uint8_t)> selected;
    void keyDownEvent(Tempest::KeyEvent& e) override;
    void keyUpEvent(Tempest::KeyEvent& e) override;
  protected:
    void paintEvent(Tempest::PaintEvent& e) override;
    void mouseDownEvent(Tempest::MouseEvent& e) override;
    void mouseUpEvent(Tempest::MouseEvent& e) override;
    void mouseMoveEvent(Tempest::MouseEvent& e) override;
  private:
    void choose();
    int rowAt(int x,int y) const;
    int rowHeight() const;
    bool active=false;
    int row=0;
};
