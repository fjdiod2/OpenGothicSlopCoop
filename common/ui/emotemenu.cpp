#include "emotemenu.h"
#include "game/emotes.h"
#include "resources.h"
#include "utils/gthfont.h"
#include <Tempest/Painter>
#include <algorithm>

using namespace Tempest;
EmoteMenu::EmoteMenu() { setVisible(false); }
void EmoteMenu::open() { active=true; row=0; setVisible(true); setFocus(true); update(); }
void EmoteMenu::close() {
  active=false; setVisible(false);
  if(owner()) owner()->setFocus(true);
  }
int EmoteMenu::rowHeight() const { return std::max(14,std::min(26,(h()-80)/14)); }
int EmoteMenu::rowAt(int x,int y) const {
  const int top=(h()-rowHeight()*14)/2;
  if(x<(w()-340)/2 || x>(w()+340)/2) return -1;
  const int i=(y-top)/rowHeight()-1;
  return y>=top+rowHeight() && i>=0 && i<12 ? i : -1;
  }
void EmoteMenu::choose() {
  const auto id=uint8_t(row+1);
  close();
  if(selected) selected(id);
  }
void EmoteMenu::keyDownEvent(KeyEvent& e) {
  if(!active) { e.ignore(); return; }
  e.accept();
  if(e.key==Event::K_ESCAPE || e.key==Event::K_V) close();
  else if(e.key==Event::K_Up) row=(row+11)%12;
  else if(e.key==Event::K_Down) row=(row+1)%12;
  else if(e.key==Event::K_Return) choose();
  else if(e.key>=Event::K_1 && e.key<=Event::K_9) { row=int(e.key-Event::K_1); choose(); }
  else if(e.key==Event::K_0) { row=9; choose(); }
  update();
  }
void EmoteMenu::keyUpEvent(KeyEvent& e) { e.accept(); }
void EmoteMenu::mouseDownEvent(MouseEvent& e) { e.accept(); }
void EmoteMenu::mouseUpEvent(MouseEvent& e) {
  e.accept();
  if(e.button==Event::ButtonRight) { close(); return; }
  const int i=rowAt(e.x,e.y);
  if(e.button==Event::ButtonLeft && i>=0) { row=i; choose(); }
  }
void EmoteMenu::mouseMoveEvent(MouseEvent& e) {
  e.accept();
  const int i=rowAt(e.x,e.y); if(i>=0) row=i;
  update();
  }
void EmoteMenu::paintEvent(PaintEvent& e) {
  if(!active) return;
  Painter p(e);
  const int rh=rowHeight(), x=(w()-340)/2,y=(h()-rh*14)/2;
  const auto& font=Resources::font(float(rh)/26.f);
  p.setBrush(Color(0.05f,0.04f,0.03f,0.94f)); p.drawRect(x-10,y-8,360,rh*14+16);
  font.drawText(p,x,y+font.pixelSize(),"Emotes");
  for(size_t i=0;i<Emotes.size();++i) {
    const int ry=y+rh*int(i+1);
    if(int(i)==row) { p.setBrush(Color(0.35f,0.25f,0.1f,1.f)); p.drawRect(x,ry,340,rh); }
    const auto label=(i<10 ? std::to_string((i+1)%10)+"  " : "    ")+std::string(Emotes[i].label);
    font.drawText(p,x+6,ry+font.pixelSize(),label);
    }
  font.drawText(p,x,y+rh*13+font.pixelSize(),"Arrows / Enter / Click    Esc: close");
  }
