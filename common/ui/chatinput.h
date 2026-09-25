#pragma once

#include <Tempest/Widget>

#include <functional>
#include <string>
#include <string_view>

// One-line text field for multiplayer chat, opened with T while a session is running.
// Enter sends the line through onSend, Escape discards it.
class ChatInput : public Tempest::Widget {
  public:
    ChatInput();

    bool isActive() const { return active; }
    void open();
    void close();

    void keyDownEvent  (Tempest::KeyEvent& e) override;
    void keyRepeatEvent(Tempest::KeyEvent& e) override;

    std::function<void(std::string_view)> onSend;

  protected:
    void paintEvent(Tempest::PaintEvent& e) override;

  private:
    bool        active = false;
    std::string text;
  };
