// SPDX-License-Identifier: MIT
#include "TerminalPanel.h"
#include "TerminalSession.h"
#include "vterm.h"

#include <Button.h>
#include <Clipboard.h>
#include <Font.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <StringView.h>
#include <Window.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace Listi {
namespace {

constexpr uint32 kPollTerminal = 'tlpt';
constexpr size_t kMaxPending = 1024 * 1024;
constexpr size_t kMaxHistory = 1000;
constexpr float kPadding = 5;
constexpr rgb_color kBackground{20, 22, 25, 255};
constexpr rgb_color kForeground{229, 231, 234, 255};

void AppendUtf8(std::string &output, uint32_t codepoint) {
  if (codepoint < 0x80) {
    output += static_cast<char>(codepoint);
    return;
  }
  const int length = codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
  char bytes[4];
  for (int index = length - 1; index > 0; --index) {
    bytes[index] = static_cast<char>(0x80 | (codepoint & 0x3f));
    codepoint >>= 6;
  }
  bytes[0] = static_cast<char>((length == 2   ? 0xc0
                                : length == 3 ? 0xe0
                                              : 0xf0) |
                               codepoint);
  output.append(bytes, length);
}

void SendUtf8(VTerm *state, const char *bytes, size_t size,
              VTermModifier mods) {
  uint32_t codepoint = 0, minimum = 0;
  unsigned remaining = 0;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t byte = bytes[i];
    if (byte < 0x80) {
      vterm_keyboard_unichar(state, byte, mods);
      remaining = 0;
    } else if ((byte & 0xc0) == 0x80 && remaining) {
      codepoint = (codepoint << 6) | (byte & 0x3f);
      if (--remaining == 0 && codepoint >= minimum && codepoint <= 0x10ffff &&
          !(codepoint >= 0xd800 && codepoint <= 0xdfff))
        vterm_keyboard_unichar(state, codepoint, mods);
    } else if (byte >= 0xc2 && byte <= 0xf4) {
      remaining = byte >= 0xf0 ? 3 : byte >= 0xe0 ? 2 : 1;
      minimum = remaining == 3 ? 0x10000 : remaining == 2 ? 0x800 : 0x80;
      codepoint = byte & (0x7f >> (remaining + 1));
    } else {
      remaining = 0;
    }
  }
}

VTermKey SpecialKey(char byte, int32 physicalKey) {
  switch (byte) {
  case B_ENTER:
    return VTERM_KEY_ENTER;
  case B_TAB:
    return VTERM_KEY_TAB;
  case B_BACKSPACE:
    return VTERM_KEY_BACKSPACE;
  case B_ESCAPE:
    return VTERM_KEY_ESCAPE;
  case B_UP_ARROW:
    return VTERM_KEY_UP;
  case B_DOWN_ARROW:
    return VTERM_KEY_DOWN;
  case B_LEFT_ARROW:
    return VTERM_KEY_LEFT;
  case B_RIGHT_ARROW:
    return VTERM_KEY_RIGHT;
  case B_INSERT:
    return VTERM_KEY_INS;
  case B_DELETE:
    return VTERM_KEY_DEL;
  case B_HOME:
    return VTERM_KEY_HOME;
  case B_END:
    return VTERM_KEY_END;
  case B_PAGE_UP:
    return VTERM_KEY_PAGEUP;
  case B_PAGE_DOWN:
    return VTERM_KEY_PAGEDOWN;
  case B_FUNCTION_KEY:
    // Haiku's physical key numbers are consecutive for F1 through F12.
    if (physicalKey >= B_F1_KEY && physicalKey <= B_F12_KEY)
      return VTermKey(VTERM_KEY_FUNCTION(physicalKey - B_F1_KEY + 1));
    break;
  }
  return VTERM_KEY_NONE;
}

} // namespace

class TerminalView : public BView {
public:
  TerminalView()
      : BView("terminal screen", B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS) {
    SetViewColor(B_TRANSPARENT_COLOR);
    SetFont(be_fixed_font);
    font_height metrics{};
    GetFontHeight(&metrics);
    ascent = std::ceil(metrics.ascent);
    rowHeight = std::ceil(ascent + metrics.descent + metrics.leading);
    columnWidth = std::ceil(StringWidth("M"));
    SetExplicitMinSize(BSize(18 * columnWidth, 5 * rowHeight));
    SetExplicitPreferredSize(BSize(38 * columnWidth, 13 * rowHeight));
  }

  ~TerminalView() override { Stop(); }
  bool Active() const { return session && session->Descriptor() >= 0; }

  int Start(const char *directory) {
    auto nextSession = std::make_unique<TerminalSession>();
    if (const int error = nextSession->Start(directory, rows, columns))
      return error;
    auto nextRunner = PollRunner(pollGeneration + 1);
    if (nextRunner->InitCheck() != B_OK)
      return EIO;
    auto *nextState = vterm_new(rows, columns);
    if (!nextState)
      return ENOMEM;

    // Only retire the current shell after its replacement can run here.
    Stop();
    session = std::move(nextSession);
    runner = std::move(nextRunner);
    state = nextState;
    history.clear();
    historyOffset = 0;
    selection = {};
    cursorVisible = true;
    alternate = false;
    vterm_set_utf8(state, 1);
    vterm_output_set_callback(state, Output, this);
    screen = vterm_obtain_screen(state);
    vterm_screen_enable_altscreen(screen, true);
    vterm_screen_set_callbacks(screen, &Callbacks(), this);
    VTermColor foreground, background;
    vterm_color_rgb(&foreground, kForeground.red, kForeground.green,
                    kForeground.blue);
    vterm_color_rgb(&background, kBackground.red, kBackground.green,
                    kBackground.blue);
    vterm_screen_set_default_colors(screen, &foreground, &background);
    vterm_screen_reset(screen, 1);
    Invalidate();
    return 0;
  }

  void Stop() {
    runner.reset();
    ++pollGeneration;
    session.reset();
    pending.clear();
    if (state)
      vterm_free(state);
    state = nullptr;
    screen = nullptr;
    Invalidate();
  }

  void AttachedToWindow() override {
    BView::AttachedToWindow();
    if (Active())
      runner = PollRunner(++pollGeneration);
  }

  void DetachedFromWindow() override {
    runner.reset();
    ++pollGeneration;
    selecting = false;
    BView::DetachedFromWindow();
  }

  void FrameResized(float width, float height) override {
    BView::FrameResized(width, height);
    const int newColumns =
        std::clamp(int((width - 2 * kPadding) / columnWidth), 2, 512);
    const int newRows =
        std::clamp(int((height - 2 * kPadding) / rowHeight), 2, 256);
    if (columns == newColumns && rows == newRows)
      return;
    columns = newColumns;
    rows = newRows;
    if (state) {
      selection = {};
      vterm_set_size(state, rows, columns);
      if (Active())
        session->Resize(rows, columns);
      Invalidate();
    }
  }

  void MakeFocus(bool focus = true) override {
    BView::MakeFocus(focus);
    if (state) {
      if (focus)
        vterm_state_focus_in(vterm_obtain_state(state));
      else
        vterm_state_focus_out(vterm_obtain_state(state));
      Invalidate();
    }
  }

  void KeyDown(const char *bytes, int32 length) override {
    if (!HandleKey(Window()->CurrentMessage()))
      BView::KeyDown(bytes, length);
  }

  bool HandleKey(BMessage *message) {
    if (!message || message->what != B_KEY_DOWN)
      return false;
    const char *bytes = message->GetString("bytes", "");
    const int32 mods = message->GetInt32("modifiers", 0);
    const int32 raw = message->GetInt32("raw_char", 0);
    if (mods & B_COMMAND_KEY) {
      if (raw == 'c' || raw == 'C') {
        Copy();
        return true;
      }
      if (raw == 'v' || raw == 'V') {
        Paste();
        return true;
      }
      // While the terminal has focus, do not invoke destructive file shortcuts.
      return bytes[0] != B_ESCAPE && raw != 't' && raw != 'T' && raw != 'q' &&
             raw != 'Q';
    }
    if ((mods & B_SHIFT_KEY) &&
        (bytes[0] == B_PAGE_UP || bytes[0] == B_PAGE_DOWN)) {
      ScrollHistory(bytes[0] == B_PAGE_UP ? rows : -rows);
      return true;
    }
    if (!Active())
      return true;
    historyOffset = 0;
    selection = {};
    int vtMods = VTERM_MOD_NONE;
    if (mods & B_OPTION_KEY)
      vtMods |= VTERM_MOD_ALT;
    if (mods & B_SHIFT_KEY)
      vtMods |= VTERM_MOD_SHIFT;
    const size_t byteCount = strlen(bytes);
    const auto key = SpecialKey(bytes[0], message->GetInt32("key", 0));
    if ((mods & B_CONTROL_KEY) && raw >= ' ' && raw < 127)
      vterm_keyboard_unichar(state, raw,
                             VTermModifier(vtMods | VTERM_MOD_CTRL));
    else if (key != VTERM_KEY_NONE && byteCount <= 1)
      vterm_keyboard_key(state, key, VTermModifier(vtMods));
    else
      SendUtf8(state, bytes, byteCount, VTermModifier(vtMods));
    FlushInput();
    Invalidate();
    return true;
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kPollTerminal:
      if (message->GetInt64("poll generation", -1) == pollGeneration)
        Poll();
      break;
    case B_COPY:
      Copy();
      break;
    case B_PASTE:
      Paste();
      break;
    case B_MOUSE_WHEEL_CHANGED: {
      const float delta = message->GetFloat("be:wheel_delta_y", 0);
      if (delta != 0)
        ScrollHistory(delta > 0 ? -3 : 3);
      break;
    }
    default:
      BView::MessageReceived(message);
    }
  }

  void MouseDown(BPoint point) override {
    MakeFocus();
    selection.start = selection.end = CellIndex(point);
    selecting = true;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
  }

  void MouseMoved(BPoint point, uint32, const BMessage *) override {
    if (selecting) {
      selection.end = CellIndex(point);
      Invalidate();
    }
  }

  void MouseUp(BPoint) override { selecting = false; }

  void Draw(BRect update) override {
    SetHighColor(kBackground);
    FillRect(update);
    if (!screen)
      return;

    for (int row = 0; row < rows; ++row) {
      const float y = kPadding + row * rowHeight;
      if (y > update.bottom || y + rowHeight < update.top)
        continue;
      for (int col = 0; col < columns; ++col) {
        const float x = kPadding + col * columnWidth;
        VTermScreenCell cell{};
        if (!Cell(row, col, cell))
          continue;
        if (cell.chars[0] == UINT32_MAX)
          continue;
        vterm_screen_convert_color_to_rgb(screen, &cell.fg);
        vterm_screen_convert_color_to_rgb(screen, &cell.bg);
        const bool selected =
            selection.start != selection.end &&
            row * columns + col >= std::min(selection.start, selection.end) &&
            row * columns + col <= std::max(selection.start, selection.end);
        if (cell.attrs.reverse != selected)
          std::swap(cell.fg, cell.bg);
        const BRect bounds(x, y,
                           x + std::max(1, int(cell.width)) * columnWidth - 1,
                           y + rowHeight - 1);
        SetHighColor(cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue);
        FillRect(bounds);
        if (cell.chars[0] && !cell.attrs.conceal) {
          std::string glyph;
          for (auto codepoint : cell.chars) {
            if (!codepoint)
              break;
            AppendUtf8(glyph, codepoint);
          }
          SetHighColor(cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue);
          DrawString(glyph.c_str(), BPoint(x, y + ascent));
          if (cell.attrs.underline)
            StrokeLine(BPoint(x, y + ascent + 1),
                       BPoint(bounds.right, y + ascent + 1));
        }
      }
    }
    if (cursorVisible && historyOffset == 0 && Active()) {
      VTermPos cursor{};
      vterm_state_get_cursorpos(vterm_obtain_state(state), &cursor);
      const float x = kPadding + cursor.col * columnWidth;
      const float y = kPadding + cursor.row * rowHeight;
      SetHighColor(kForeground);
      if (IsFocus())
        FillRect(BRect(x, y + rowHeight - 2, x + columnWidth - 1,
                       y + rowHeight - 1));
      else
        StrokeRect(BRect(x, y, x + columnWidth - 1, y + rowHeight - 1));
    }
  }

private:
  std::unique_ptr<BMessageRunner> PollRunner(int64 generation) {
    BMessage tick(kPollTerminal);
    tick.AddInt64("poll generation", generation);
    return std::make_unique<BMessageRunner>(BMessenger(this), &tick, 30000);
  }

  struct Selection {
    int start = 0;
    int end = 0;
  } selection;

  static const VTermScreenCallbacks &Callbacks() {
    static const VTermScreenCallbacks callbacks = [] {
      VTermScreenCallbacks result{};
      result.settermprop = [](VTermProp prop, VTermValue *value, void *data) {
        auto &view = *static_cast<TerminalView *>(data);
        if (prop == VTERM_PROP_CURSORVISIBLE)
          view.cursorVisible = value->boolean;
        else if (prop == VTERM_PROP_ALTSCREEN) {
          view.alternate = value->boolean;
          view.historyOffset = 0;
        }
        return 1;
      };
      result.sb_pushline = [](int continuation, int cols, const VTermScreenCell *cells,
                              void *data) {
        auto &view = *static_cast<TerminalView *>(data);
        view.history.push_back({{cells, cells + cols}, continuation != 0});
        if (view.history.size() > kMaxHistory)
          view.history.pop_front();
        if (view.historyOffset > 0)
          view.historyOffset =
              std::min(view.historyOffset + 1, int(view.history.size()));
        return 1;
      };
      result.sb_clear = [](void *data) {
        auto &view = *static_cast<TerminalView *>(data);
        view.history.clear();
        view.historyOffset = 0;
        return 1;
      };
      return result;
    }();
    return callbacks;
  }

  static void Output(const char *bytes, size_t size, void *data) {
    auto &view = *static_cast<TerminalView *>(data);
    if (size <= kMaxPending - view.pending.size())
      view.pending.append(bytes, size);
  }

  void FlushInput() {
    if (pending.empty() || !Active())
      return;
    const ssize_t written =
        write(session->Descriptor(), pending.data(), pending.size());
    if (written > 0)
      pending.erase(0, written);
    else if (written < 0 && errno != EINTR && errno != EAGAIN &&
             errno != EWOULDBLOCK)
      pending.clear();
  }

  void Poll() {
    if (!Active())
      return;
    char data[8192];
    bool changed = false;
    for (int batch = 0; batch < 16; ++batch) {
      const ssize_t count = read(session->Descriptor(), data, sizeof(data));
      if (count > 0) {
        vterm_input_write(state, data, count);
        changed = true;
        continue;
      }
      if (count < 0 && errno == EINTR)
        continue;
      if (count == 0 ||
          (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ||
          session->Reap()) {
        session.reset();
        runner.reset();
        static const char ended[] = "\r\n[Listi CLI session ended]";
        vterm_input_write(state, ended, sizeof(ended) - 1);
        changed = true;
      }
      break;
    }
    FlushInput();
    if (changed) {
      vterm_screen_flush_damage(screen);
      Invalidate();
    }
  }

  bool Cell(int row, int col, VTermScreenCell &cell) const {
    row -= historyOffset;
    if (row >= 0)
      return vterm_screen_get_cell(screen, VTermPos{row, col}, &cell);
    const auto &line = history[history.size() + row].cells;
    if (size_t(col) >= line.size())
      return false;
    cell = line[col];
    return true;
  }

  void ScrollHistory(int amount) {
    if (alternate)
      return;
    historyOffset = std::clamp(historyOffset + amount, 0, int(history.size()));
    selection = {};
    Invalidate();
  }

  int CellIndex(BPoint point) const {
    const int row =
        std::clamp(int((point.y - kPadding) / rowHeight), 0, rows - 1);
    const int col =
        std::clamp(int((point.x - kPadding) / columnWidth), 0, columns - 1);
    return row * columns + col;
  }

  bool ContinuesPreviousLine(int row) const {
    row -= historyOffset;
    if (row < 0)
      return history[history.size() + row].continuation;
    return vterm_state_get_lineinfo(vterm_obtain_state(state), row)->continuation;
  }

  void Copy() {
    if (!screen || selection.start == selection.end)
      return;
    std::string text;
    const int first = std::min(selection.start, selection.end);
    const int last = std::max(selection.start, selection.end);
    for (int index = first; index <= last; ++index) {
      if (index != first && index % columns == 0 &&
          !ContinuesPreviousLine(index / columns)) {
        while (!text.empty() && text.back() == ' ')
          text.pop_back();
        text += '\n';
      }
      VTermScreenCell cell{};
      if (!Cell(index / columns, index % columns, cell) || !cell.chars[0]) {
        text += ' ';
        continue;
      }
      if (cell.chars[0] == UINT32_MAX)
        continue;
      for (auto codepoint : cell.chars) {
        if (!codepoint)
          break;
        AppendUtf8(text, codepoint);
      }
    }
    if (!be_clipboard->Lock())
      return;
    be_clipboard->Clear();
    if (auto *message = be_clipboard->Data()) {
      message->AddData("text/plain", B_MIME_TYPE, text.data(), text.size());
      be_clipboard->Commit();
    }
    be_clipboard->Unlock();
  }

  void Paste() {
    if (!Active() || !be_clipboard->Lock())
      return;
    const void *data;
    ssize_t size;
    auto *message = be_clipboard->Data();
    if (message &&
        message->FindData("text/plain", B_MIME_TYPE, &data, &size) == B_OK &&
        size >= 0 && size_t(size) + 32 <= kMaxPending - pending.size()) {
      // Clipboard text may contain raw terminal control bytes, including an
      // escape sequence that would end bracketed paste prematurely.
      const auto *bytes = static_cast<const char *>(data);
      std::string text;
      text.reserve(size);
      for (ssize_t index = 0; index < size; ++index) {
        const auto byte = static_cast<uint8_t>(bytes[index]);
        if (byte == '\r' || byte == '\n') {
          if (byte != '\r' || index + 1 == size || bytes[index + 1] != '\n')
            text += '\r';
        } else if (byte == '\t' || (byte >= ' ' && byte != 127)) {
          text += bytes[index];
        }
      }
      vterm_keyboard_start_paste(state);
      SendUtf8(state, text.data(), text.size(), VTERM_MOD_NONE);
      vterm_keyboard_end_paste(state);
    }
    be_clipboard->Unlock();
    historyOffset = 0;
    selection = {};
    FlushInput();
    Invalidate();
  }

  std::unique_ptr<TerminalSession> session;
  VTerm *state = nullptr;
  VTermScreen *screen = nullptr;
  std::unique_ptr<BMessageRunner> runner;
  int64 pollGeneration = 0;
  std::string pending;
  struct HistoryLine {
    std::vector<VTermScreenCell> cells;
    bool continuation;
  };
  std::deque<HistoryLine> history;
  int historyOffset = 0;
  int rows = 12;
  int columns = 80;
  float ascent = 0;
  float rowHeight = 0;
  float columnWidth = 0;
  bool cursorVisible = true;
  bool alternate = false;
  bool selecting = false;
};

TerminalPanel::TerminalPanel(BMessenger owner, int lister)
    : BGroupView(B_VERTICAL, 0),
      header(new BGroupLayout(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)),
      location(new BStringView("terminal location", TerminalName(lister))),
      terminal(new TerminalView()),
      position(new BButton("terminal position", "Move right", nullptr)),
      move(new BButton("Detach CLI", nullptr)),
      close(new BButton("Close CLI", nullptr)) {
  location->SetTruncation(B_TRUNCATE_END);
  location->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
  location->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  BLayoutBuilder::Group<>(this)
      .AddGroup(header, 0)
      .SetInsets(B_USE_HALF_ITEM_SPACING, 0)
      .AddGroup(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
      .Add(location)
      .Add(position)
      .End()
      .AddGroup(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING, 0)
      .Add(move)
      .Add(close)
      .End()
      .End()
      .Add(terminal);
  SetDockSide(TerminalSide::Below);
  SetOwner(owner, lister);
  position->SetTimeout(0);
  move->SetTimeout(0);
  close->SetTimeout(0);
}

void TerminalPanel::SetOwner(BMessenger owner, int lister) {
  SetName(TerminalName(lister));
  RefreshLocation();
  for (auto *button : {position, move, close})
    button->SetTarget(owner);
}

std::string TerminalPanel::DisplayTitle() const {
  return tabLabel.empty() ? std::string(Name()) : std::string(Name()) + " | " + tabLabel;
}

void TerminalPanel::SetTabLabel(const std::string &label) {
  tabLabel = label;
  RefreshLocation();
}

void TerminalPanel::RefreshLocation() {
  BString title(DisplayTitle().c_str());
  if (!startFolder.empty())
    title << "   Started in: " << startFolder.c_str();
  location->SetText(title);
  location->SetToolTip(title);
}

void TerminalPanel::SetDockSide(TerminalSide side) {
  dockSide = side;
  header->SetOrientation(IsSideDock(side) && !floating ? B_VERTICAL
                                                         : B_HORIZONTAL);
  switch (side) {
  case TerminalSide::Below:
    position->SetLabel("Move right");
    break;
  case TerminalSide::Right:
    position->SetLabel("Move above");
    break;
  case TerminalSide::Above:
    position->SetLabel("Move left");
    break;
  case TerminalSide::Left:
    position->SetLabel("Move below");
    break;
  }
  position->SetToolTip("Move the shared CLI dock.");
}

void TerminalPanel::SetPlacement(bool detached, int64 generation) {
  placement = generation;
  if (floating != detached) {
    floating = detached;
    header->SetOrientation(IsSideDock(dockSide) && !floating ? B_VERTICAL : B_HORIZONTAL);
    if (floating)
      position->Hide();
    else
      position->Show();
  }
  auto reposition = new BMessage(kToggleTerminalSide);
  reposition->AddInt64("terminal placement", placement);
  position->SetMessage(reposition);
  move->SetLabel(floating ? "Dock CLI" : "Detach CLI");
  auto change = new BMessage(floating ? kDockTerminal : kFloatTerminal);
  change->AddInt64("terminal placement", placement);
  move->SetMessage(change);
  auto stop = new BMessage(kCloseTerminal);
  stop->AddInt64("terminal placement", placement);
  close->SetMessage(stop);
}

bool TerminalPanel::Active() const { return terminal->Active(); }
bool TerminalPanel::HasFocus() const { return terminal->IsFocus(); }
void TerminalPanel::Focus() { terminal->MakeFocus(); }

int TerminalPanel::Start(const char *path) {
  const int error = terminal->Start(path);
  if (error)
    return error;
  ++sessionId;
  startFolder = path;
  RefreshLocation();
  return 0;
}

void TerminalPanel::Stop() { terminal->Stop(); }

bool TerminalPanel::HandleKey(BMessage *message) {
  return terminal->HandleKey(message);
}

} // namespace Listi
