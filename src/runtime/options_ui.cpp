#include "runtime/options_ui.h"

#include <cstring>

namespace asc::ui {
namespace {

constexpr int W = 640, H = 480;

// Colours chosen to match the game's menu buttons and stone frames.
constexpr uint32_t kNavy = 0xFF0E1C2A;        // panel fill
constexpr uint32_t kButton = 0xFF102436;      // button fill
constexpr uint32_t kButtonHover = 0xFF1A4A5A;
constexpr uint32_t kButtonOn = 0xFF1F5F6F;    // selected (like a highlighted menu button)
constexpr uint32_t kStoneLight = 0xFF7FA6AA;
constexpr uint32_t kStone = 0xFF5A8086;
constexpr uint32_t kStoneDark = 0xFF26383C;
constexpr uint32_t kText = 0xFFE6E6E6;
constexpr uint32_t kTitle = 0xFFE0CC88;
constexpr uint32_t kLabel = 0xFF7FD0A0;       // the game's green
constexpr uint32_t kDim = 0xFF5E6B70;
constexpr uint32_t kDesc = 0xFFAEB8BC;

const Rect kPanel{100, 40, 440, 400};

void Fill(uint32_t* p, Rect r, uint32_t c) {
  for (int y = r.y; y < r.y + r.h; ++y) {
    if (y < 0 || y >= H) continue;
    for (int x = r.x; x < r.x + r.w; ++x)
      if (x >= 0 && x < W) p[y * W + x] = c;
  }
}

// Stone-style frame: light outer edge, mid band, dark inner edge, with a
// little deterministic speckle like the game's granite borders.
void Frame(uint32_t* p, Rect r, int thickness) {
  for (int t = 0; t < thickness; ++t) {
    uint32_t base = t == 0 ? kStoneLight : t == thickness - 1 ? kStoneDark : kStone;
    Rect e{r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t};
    for (int x = e.x; x < e.x + e.w; ++x) {
      for (int y : {e.y, e.y + e.h - 1}) {
        if (x < 0 || x >= W || y < 0 || y >= H) continue;
        uint32_t c = base;
        if (t > 0 && t < thickness - 1 && ((x * 7 + y * 13) % 11) == 0) c = kStoneDark;
        p[y * W + x] = c;
      }
    }
    for (int y = e.y; y < e.y + e.h; ++y) {
      for (int x : {e.x, e.x + e.w - 1}) {
        if (x < 0 || x >= W || y < 0 || y >= H) continue;
        uint32_t c = base;
        if (t > 0 && t < thickness - 1 && ((x * 13 + y * 7) % 11) == 0) c = kStoneDark;
        p[y * W + x] = c;
      }
    }
  }
}

void Text(uint32_t* p, const FntFont* f, const std::string& s, int x, int y, uint32_t c) {
  if (!f) return;
  f->Draw(s, x, y, [&](int px, int py) {
    if (px >= 0 && px < W && py >= 0 && py < H) p[py * W + px] = c;
  });
}

void TextCentered(uint32_t* p, const FntFont* f, const std::string& s, Rect r, uint32_t c) {
  if (!f) return;
  Text(p, f, s, r.x + (r.w - f->TextWidth(s)) / 2, r.y + (r.h - f->height()) / 2, c);
}

void Button(uint32_t* p, const FntFont* f, Rect r, const std::string& label, bool on, bool hover,
            bool disabled = false) {
  Fill(p, r, on ? kButtonOn : hover && !disabled ? kButtonHover : kButton);
  Frame(p, r, 2);
  TextCentered(p, f, label, r, disabled ? kDim : kText);
}

}  // namespace

Rect OptionsPanel::ItemRect(int id) {
  const int col1 = 250, col2 = 386, arrowR = 486;
  switch (id) {
    case kFullscreen: return {col1, 84, 126, 22};
    case kWindow: return {col2, 84, 126, 22};
    case kFilterPrev: return {col1, 120, 26, 22};
    case kFilterNext: return {arrowR, 120, 26, 22};
    case kHelpOn: return {col1, 166, 126, 22};
    case kHelpOff: return {col2, 166, 126, 22};
    case kAspect43: return {col1, 202, 126, 22};
    case kMusicDown: return {col1, 246, 26, 22};
    case kMusicUp: return {arrowR, 246, 26, 22};
    case kFxDown: return {col1, 282, 26, 22};
    case kFxUp: return {arrowR, 282, 26, 22};
    case kClickPrev: return {col1, 318, 26, 22};
    case kClickNext: return {arrowR, 318, 26, 22};
    case kClose: return {270, 386, 100, 24};
    default: return {0, 0, 0, 0};
  }
}

bool OptionsPanel::OnMove(int x, int y) {
  int h = -1;
  if (open_)
    for (int i = 0; i < kCount; ++i)
      if (ItemRect(i).Contains(x, y)) h = i;
  bool changed = h != hover_;
  hover_ = h;
  return changed;
}

Action OptionsPanel::OnClick(int x, int y, Settings& s) {
  if (!open_) return Action::kNone;
  int id = -1;
  for (int i = 0; i < kCount; ++i)
    if (ItemRect(i).Contains(x, y)) id = i;
  const int n = int(Filter::kCount);
  switch (id) {
    case kFullscreen: if (!s.fullscreen) { s.fullscreen = true; return Action::kChanged; } break;
    case kWindow: if (s.fullscreen) { s.fullscreen = false; return Action::kChanged; } break;
    case kFilterPrev: s.filter = Filter((int(s.filter) + n - 1) % n); return Action::kChanged;
    case kFilterNext: s.filter = Filter((int(s.filter) + 1) % n); return Action::kChanged;
    case kHelpOn: if (!s.rightClickHelp) { s.rightClickHelp = true; return Action::kChanged; } break;
    case kHelpOff: if (s.rightClickHelp) { s.rightClickHelp = false; return Action::kChanged; } break;
    case kMusicDown: if (s.musicVolume > 0) { --s.musicVolume; return Action::kChanged; } break;
    case kMusicUp: if (s.musicVolume < 10) { ++s.musicVolume; return Action::kChanged; } break;
    case kFxDown: if (s.effectsVolume > 0) { --s.effectsVolume; return Action::kChanged; } break;
    case kFxUp: if (s.effectsVolume < 10) { ++s.effectsVolume; return Action::kChanged; } break;
    case kClickPrev: {
      const int c = int(ClickSound::kCount);
      s.clickSound = ClickSound((int(s.clickSound) + c - 1) % c);
      return Action::kChanged;
    }
    case kClickNext:
      s.clickSound = ClickSound((int(s.clickSound) + 1) % int(ClickSound::kCount));
      return Action::kChanged;
    case kClose: return Action::kClose;
    default:
      if (!kPanel.Contains(x, y)) return Action::kClose;  // click outside closes
  }
  return Action::kNone;
}

void OptionsPanel::Draw(uint32_t* p, const Settings& s, bool menuButton, bool menuButtonHover) const {
  std::memset(p, 0, size_t(W) * H * 4);
  if (menuButton && !open_) Button(p, big_, MenuButton(), "Options", false, menuButtonHover);
  if (!open_) return;

  // Dim the game behind the panel.
  for (int i = 0; i < W * H; ++i) p[i] = 0x88000000u;
  Fill(p, kPanel, kNavy);
  Frame(p, kPanel, 4);
  TextCentered(p, big_, "Ascendancy Port Options", {kPanel.x, kPanel.y + 10, kPanel.w, 16}, kTitle);
  Fill(p, {kPanel.x + 20, kPanel.y + 32, kPanel.w - 40, 1}, kStone);

  auto label = [&](const char* t, Rect r) {
    Text(p, big_, t, 122, r.y + (r.h - (big_ ? big_->height() : 0)) / 2, kLabel);
  };
  auto hov = [&](int id) { return hover_ == id; };

  label("Display", ItemRect(kFullscreen));
  Button(p, big_, ItemRect(kFullscreen), "Fullscreen", s.fullscreen, hov(kFullscreen));
  Button(p, big_, ItemRect(kWindow), "Window", !s.fullscreen, hov(kWindow));

  Rect prev = ItemRect(kFilterPrev), next = ItemRect(kFilterNext);
  label("Picture", prev);
  Button(p, big_, prev, "<", false, hov(kFilterPrev));
  Button(p, big_, next, ">", false, hov(kFilterNext));
  Rect name{prev.x + prev.w + 6, prev.y, next.x - prev.x - prev.w - 12, prev.h};
  Fill(p, name, kButtonOn);
  Frame(p, name, 2);
  TextCentered(p, big_, FilterName(s.filter), name, kText);
  TextCentered(p, small_, FilterDescription(s.filter), {prev.x - 10, prev.y + 26, next.x + next.w - prev.x + 20, 12}, kDesc);

  label("Right-click", ItemRect(kHelpOn));
  Button(p, big_, ItemRect(kHelpOn), "Help popup", s.rightClickHelp, hov(kHelpOn));
  Button(p, big_, ItemRect(kHelpOff), "Normal click", !s.rightClickHelp, hov(kHelpOff));

  label("Aspect", ItemRect(kAspect43));
  Button(p, big_, ItemRect(kAspect43), "4:3", true, false);
  Rect wide = ItemRect(kAspect43);
  wide.x += 136;
  Button(p, big_, wide, "16:9 (soon)", false, false, true);

  // Sound: two volume bars and the click sound.
  auto valueBox = [&](int prevId, int nextId) {
    Rect a = ItemRect(prevId), b = ItemRect(nextId);
    return Rect{a.x + a.w + 6, a.y, b.x - a.x - a.w - 12, a.h};
  };
  auto volume = [&](const char* name, int downId, int upId, int value) {
    label(name, ItemRect(downId));
    Button(p, big_, ItemRect(downId), "-", false, hov(downId) && value > 0, value == 0);
    Button(p, big_, ItemRect(upId), "+", false, hov(upId) && value < 10, value == 10);
    Rect box = valueBox(downId, upId);
    Fill(p, box, kButton);
    Frame(p, box, 2);
    if (value == 0) {
      TextCentered(p, big_, "Off", box, kDim);
      return;
    }
    const int segW = (box.w - 8 - 9 * 3) / 10;
    const int x0 = box.x + (box.w - (segW * 10 + 9 * 3)) / 2;
    for (int i = 0; i < 10; ++i)
      Fill(p, {x0 + i * (segW + 3), box.y + 6, segW, box.h - 12}, i < value ? kLabel : kStoneDark);
  };
  Fill(p, {kPanel.x + 20, 236, kPanel.w - 40, 1}, kStoneDark);
  volume("Music", kMusicDown, kMusicUp, s.musicVolume);
  volume("Effects", kFxDown, kFxUp, s.effectsVolume);

  label("Clicks", ItemRect(kClickPrev));
  Button(p, big_, ItemRect(kClickPrev), "<", false, hov(kClickPrev));
  Button(p, big_, ItemRect(kClickNext), ">", false, hov(kClickNext));
  Rect cbox = valueBox(kClickPrev, kClickNext);
  Fill(p, cbox, kButtonOn);
  Frame(p, cbox, 2);
  TextCentered(p, big_, ClickSoundName(s.clickSound), cbox, kText);
  Rect cp = ItemRect(kClickPrev), cn = ItemRect(kClickNext);
  TextCentered(p, small_, ClickSoundDescription(s.clickSound),
               {cp.x - 10, cp.y + 26, cn.x + cn.w - cp.x + 20, 12}, kDesc);

  Button(p, big_, ItemRect(kClose), "Close", false, hov(kClose));
  TextCentered(p, small_, "F12 opens this panel at any time  -  Esc closes it",
               {kPanel.x, kPanel.y + kPanel.h - 16, kPanel.w, 10}, kDesc);
}

}  // namespace asc::ui
