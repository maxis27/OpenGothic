#include "chatinput.h"

#include <Tempest/Painter>
#include <Tempest/Application>

#include "net/netprotocol.h"
#include "utils/gthfont.h"
#include "resources.h"
#include "gothic.h"

using namespace Tempest;

ChatInput::ChatInput() {
  setVisible(false);
  }

void ChatInput::open() {
  text.clear();
  active = true;
  setVisible(true);
  update();
  }

void ChatInput::close() {
  active = false;
  text.clear();
  setVisible(false);
  update();
  }

void ChatInput::keyDownEvent(KeyEvent& e) {
  e.accept();
  if(e.key==Event::K_ESCAPE) {
    close();
    return;
    }
  if(e.key==Event::K_Return) {
    const std::string line = std::move(text);
    close();
    if(!line.empty() && onSend)
      onSend(line);
    return;
    }
  if(e.key==Event::K_Back) {
    if(!text.empty())
      text.pop_back();
    update();
    return;
    }
  // game fonts cover ASCII only
  if(e.code<0x20 || e.code>0x7E || text.size()>=NetProtocol::MaxChatLength)
    return;
  text.push_back(char(e.code));
  update();
  }

void ChatInput::keyRepeatEvent(KeyEvent& e) {
  keyDownEvent(e);
  }

void ChatInput::paintEvent(PaintEvent& e) {
  if(!active)
    return;

  Painter     p(e);
  const float scale = Gothic::interfaceScale(this);
  auto&       fnt   = Resources::font(scale);
  const int   pad   = int(8*scale);
  const int   x     = int(20*scale);
  const int   y     = h()*3/4;
  const int   lineH = fnt.pixelSize();

  p.setBrush(Color(0,0,0,0.5f));
  p.drawRect(x-pad, y-lineH-pad, w()/2, lineH+2*pad);

  const std::string line = "Say: " + text;
  fnt.drawText(p, x, y, line);

  // blinking cursor, as in the console
  const int   cx = x + fnt.textSize(line.data(), line.data()+line.size()).w;
  const float a  = float(Application::tickCount()%1000)<500.f ? 1.f : 0.f;
  p.setBrush(Color(1,1,1,a));
  p.drawRect(cx, y-lineH, 1, lineH);
  update();
  }
