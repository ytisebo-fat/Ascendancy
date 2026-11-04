// Miles AIL digital-audio replacement, mixed with SDL audio.
//
// What the game uses (ssystem.cpp, AIL 3.02, all cdecl):
//   AIL_install_DIG_INI()                       -> driver (we return a token)
//   AIL_allocate_sample_handle(driver)          -> sample handle (8 of them)
//   AIL_init_sample / set_sample_type / set_sample_address /
//   set_sample_playback_rate / set_sample_volume / set_sample_pan /
//   set_sample_loop_count / start_sample / stop_sample / sample_status
//   Music (handle 0): AIL_minimum_sample_buffer_size, sample_buffer_ready,
//   load_sample_buffer - 8-bit mono 22050 Hz streamed through two buffers
//   that the game refills from its frame loop.
// Effects (handles 1-7) are played from the game's effect cache: entries of
// 0x16 bytes at ssystem(0xF8638)+0xBB4, +8 = sample address, +0x14 = effect
// number in soundfx.txt (0 = "button", the click used by every button).
#include "runtime/audio.h"

#include <windows.h>

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>

#include "runtime/dos.h"
#include "runtime/game_image.h"
#include "runtime/hooks.h"

namespace asc::audio {
namespace {

// Miles status / format constants
constexpr int kSmpDone = 2, kSmpPlaying = 4, kSmpStopped = 8;
constexpr int kFmt16 = 1, kFmtStereo = 2;
constexpr int kPcmSign = 1;

constexpr uint32_t kSoundSystem = 0xF8638;   // the game's ssystem object
constexpr uint32_t kSfxCache = 0xBB4, kSfxCacheEntries = 6, kSfxCacheStride = 0x16;
constexpr int kSfxButton = 0;                 // soundfx.txt: "button"

struct Variant {
  std::vector<float> data;  // mono, 22050 Hz
  const char* name = "";
};

enum BufState : uint8_t { kEmpty, kReserved, kQueued };

struct Voice {
  int status = kSmpDone;
  int format = 0, flags = 0, rate = 11025, volume = 127, pan = 64, loops = 1;
  const uint8_t* addr[2] = {};
  uint32_t len[2] = {};
  // Double-buffer (music) state.
  bool streaming = false;
  BufState buf[2] = {kEmpty, kEmpty};
  int cur = 0, next = -1, fillIdx = -1;
  uint32_t srcPos = 0;  // byte offset of the next frame in the current buffer
  // Click variation.
  const Variant* variant = nullptr;
  float pitch = 1.0f, gain = 1.0f;
  // Resampler: the last four source frames, position between [1] and [2].
  float hist[4][2] = {};
  double frac = 1.0;
  int tail = 0;
};

struct Lock {
  Lock();
  ~Lock();
};

CRITICAL_SECTION g_cs;
Lock::Lock() { EnterCriticalSection(&g_cs); }
Lock::~Lock() { LeaveCriticalSection(&g_cs); }

GameImage* g_image = nullptr;
std::vector<std::unique_ptr<Voice>> g_voices;
Voice g_preview;  // options panel click preview (not a game sample)
SDL_AudioDeviceID g_dev = 0;
int g_devRate = 48000;
uint32_t g_driverToken[16];

std::atomic<float> g_musicGain{0.49f}, g_sfxGain{0.64f};
std::atomic<int> g_clickMode{int(ClickSound::kVaried)};
std::vector<Variant> g_clicks;  // for ClickSound::kMixed
std::mt19937 g_rng{GetTickCount()};
int g_lastStep = 99, g_lastClick = -1;
unsigned g_underruns = 0;

Voice* V(int h) {
  for (auto& v : g_voices)
    if (reinterpret_cast<intptr_t>(v.get()) == h) return v.get();
  return nullptr;
}

float Gain10(int v) {
  if (v <= 0) return 0.0f;
  float x = float(v > 10 ? 10 : v) / 10.0f;
  return x * x;
}

// ---------------------------------------------------------------- mixing

// Reads the next source frame; false at the end of the data (or, for the
// music stream, when the game has not refilled the next buffer yet).
bool NextFrame(Voice& v, float out[2]) {
  if (v.variant) {
    if (v.srcPos >= v.variant->data.size()) return false;
    out[0] = out[1] = v.variant->data[v.srcPos++];
    return true;
  }
  const int bytes = (v.format & kFmt16 ? 2 : 1) * (v.format & kFmtStereo ? 2 : 1);
  for (int guard = 0; guard < 4; ++guard) {
    int b = v.streaming ? v.cur : 0;
    if (b < 0) return false;
    if (v.addr[b] && v.srcPos + bytes <= v.len[b]) {
      const uint8_t* p = v.addr[b] + v.srcPos;
      v.srcPos += bytes;
      for (int c = 0; c < (v.format & kFmtStereo ? 2 : 1); ++c) {
        float s;
        if (v.format & kFmt16) {
          int16_t w;
          std::memcpy(&w, p + c * 2, 2);
          s = w / 32768.0f;
        } else {
          int x = p[c];
          s = (v.flags & kPcmSign) ? int8_t(x) / 128.0f : (x - 128) / 128.0f;
        }
        out[c] = s;
      }
      if (!(v.format & kFmtStereo)) out[1] = out[0];
      return true;
    }
    if (v.streaming) {
      // End of the current buffer: move on to the other one if the game has
      // loaded it; if this is the buffer the game is still filling, wait.
      if (v.next >= 0) {
        v.buf[b] = kEmpty;
        v.cur = v.next;
        v.next = -1;
        v.srcPos = 0;
        continue;
      }
      if (b != v.fillIdx) { v.buf[b] = kEmpty; v.cur = -1; v.srcPos = 0; }
      return false;
    }
    if (v.loops != 1 && v.len[0] > 0) {
      if (v.loops > 1) --v.loops;
      v.srcPos = 0;
      continue;
    }
    return false;
  }
  return false;
}

inline float Hermite(float x0, float x1, float x2, float x3, float t) {
  float c1 = 0.5f * (x2 - x0);
  float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
  float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
  return ((c3 * t + c2) * t + c1) * t + x1;
}

void MixVoice(Voice& v, float* out, int frames, bool music) {
  float g = (music ? g_musicGain.load() : g_sfxGain.load()) * v.gain * float(v.volume) / 127.0f;
  float pl = float(127 - v.pan) / 63.5f, pr = float(v.pan) / 63.5f;
  float gl = g * (pl > 1.0f ? 1.0f : pl), gr = g * (pr > 1.0f ? 1.0f : pr);
  int srcRate = v.variant ? 22050 : v.rate;
  double step = double(srcRate) * v.pitch / g_devRate;
  for (int i = 0; i < frames; ++i) {
    while (v.frac >= 1.0) {
      std::memmove(v.hist[0], v.hist[1], sizeof(float) * 6);
      float f[2] = {0, 0};
      if (!NextFrame(v, f)) {
        if (v.streaming) {
          if (v.tail == 0) ++g_underruns;
          v.tail = 1;
        } else if (++v.tail >= 3) {
          v.status = kSmpDone;
          return;
        }
      } else {
        v.tail = 0;
      }
      v.hist[3][0] = f[0];
      v.hist[3][1] = f[1];
      v.frac -= 1.0;
    }
    float t = float(v.frac);
    out[2 * i] += gl * Hermite(v.hist[0][0], v.hist[1][0], v.hist[2][0], v.hist[3][0], t);
    out[2 * i + 1] += gr * Hermite(v.hist[0][1], v.hist[1][1], v.hist[2][1], v.hist[3][1], t);
    v.frac += step;
  }
}

inline float SoftClip(float x) {
  const float knee = 0.8f;
  float a = std::fabs(x);
  if (a <= knee) return x;
  float y = knee + (1.0f - knee) * std::tanh((a - knee) / (1.0f - knee));
  return x < 0 ? -y : y;
}

void SDLCALL AudioCallback(void*, Uint8* stream, int bytes) {
  float* out = reinterpret_cast<float*>(stream);
  int frames = bytes / int(2 * sizeof(float));
  std::memset(stream, 0, size_t(bytes));
  {
    Lock lock;
    for (auto& v : g_voices)
      if (v->status == kSmpPlaying) MixVoice(*v, out, frames, v->streaming);
    if (g_preview.status == kSmpPlaying) MixVoice(g_preview, out, frames, false);
  }
  for (int i = 0; i < 2 * frames; ++i) out[i] = SoftClip(out[i]);
}

void ResetPlayback(Voice& v) {
  std::memset(v.hist, 0, sizeof v.hist);
  v.frac = 1.0;
  v.tail = 0;
  v.srcPos = 0;
}

// ------------------------------------------------------- click variation

int SfxNumberOf(const uint8_t* addr) {
  uint32_t ss = g_image->Host(kSoundSystem);
  for (uint32_t i = 0; i < kSfxCacheEntries; ++i) {
    uint32_t e = ss + kSfxCache + i * kSfxCacheStride;
    uint32_t a;
    std::memcpy(&a, reinterpret_cast<const void*>(uintptr_t(e + 8)), 4);
    if (a == uint32_t(uintptr_t(addr))) {
      int16_t n;
      std::memcpy(&n, reinterpret_cast<const void*>(uintptr_t(e + 0x14)), 2);
      return n;
    }
  }
  return -1;
}

float Uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

void VaryClick(Voice& v) {
  v.variant = nullptr;
  v.pitch = 1.0f;
  v.gain = 1.0f;
  int mode = g_clickMode.load();
  if (mode == int(ClickSound::kOriginal)) return;
  if (mode == int(ClickSound::kMixed) && g_clicks.size() >= 2) {
    int n = int(g_clicks.size());
    int pick = std::uniform_int_distribution<int>(0, n - 2)(g_rng);
    if (pick >= g_lastClick && g_lastClick >= 0) ++pick;  // never the same one twice in a row
    g_lastClick = pick;
    v.variant = &g_clicks[pick];
    v.pitch = std::pow(2.0f, Uniform(-0.3f, 0.3f) / 12.0f);
    v.gain = Uniform(0.88f, 1.0f);
    return;
  }
  // Varied: seven pitch steps 0.35 semitone apart (about +-6%), never the
  // same step twice in a row, and a little volume variation.
  int step = std::uniform_int_distribution<int>(-3, 2)(g_rng);
  if (step >= g_lastStep) ++step;
  g_lastStep = step;
  v.pitch = std::pow(2.0f, step * 0.35f / 12.0f);
  v.gain = Uniform(0.82f, 1.0f);
}

// Decodes a .voc from the archive the way the game does (raw files: skip the
// 42-byte "header" it assumes), trims silence, caps the length with a fade,
// and matches loudness to `targetRms` (0 = keep).
bool LoadClick(const Config& cfg, const char* name, float maxMs, Variant* out) {
  std::vector<uint8_t> f;
  std::string path = std::string("data\\") + name + ".voc";
  if (!cfg.readGameFile || !cfg.readGameFile(path.c_str(), &f) || f.size() < 64) return false;
  size_t off = 0x2A, len = f.size() - 0x2B;
  if (!std::memcmp(f.data(), "Creative Voice File", 19)) {
    size_t blk = f[20] | (f[21] << 8);
    if (blk + 16 > f.size()) return false;
    uint32_t blen = f[blk + 1] | (f[blk + 2] << 8) | (f[blk + 3] << 16);
    if (f[blk] == 9) {
      uint32_t rate;
      std::memcpy(&rate, &f[blk + 4], 4);
      if (rate != 22050 || f[blk + 8] != 8 || f[blk + 9] != 1) return false;
      off = blk + 16;
      len = blen - 12;
    } else {
      return false;
    }
    if (off + len > f.size()) len = f.size() - off;
  }
  std::vector<float> d(len);
  float peak = 0;
  for (size_t i = 0; i < len; ++i) {
    d[i] = (int(f[off + i]) - 128) / 128.0f;
    peak = std::fmax(peak, std::fabs(d[i]));
  }
  size_t start = 0;
  while (start < d.size() && std::fabs(d[start]) < peak * 0.02f) ++start;
  if (start > 0) start -= std::min<size_t>(start, 22);  // keep 1 ms before the attack
  d.erase(d.begin(), d.begin() + long(start));
  size_t maxLen = size_t(22050 * maxMs / 1000);
  if (d.size() > maxLen) d.resize(maxLen);
  size_t fade = std::min<size_t>(d.size() / 3, 22050 * 40 / 1000);
  for (size_t i = 0; i < fade; ++i) d[d.size() - 1 - i] *= float(i) / float(fade);
  for (size_t i = 0; i < 22 && i < d.size(); ++i) d[i] *= float(i) / 22.0f;
  out->data = std::move(d);
  out->name = name;
  return true;
}

float Rms(const std::vector<float>& d) {
  double s = 0;
  for (float x : d) s += double(x) * x;
  return d.empty() ? 0.0f : float(std::sqrt(s / double(d.size())));
}

// ------------------------------------------------------------ AIL entries

intptr_t __cdecl InstallDigIni() {
  Log("audio: AIL_install_DIG_INI -> %s", g_dev ? "port mixer" : "no audio device");
  return g_dev ? reinterpret_cast<intptr_t>(g_driverToken) : 0;
}

intptr_t __cdecl InstallDigDriverFile(const char*, void*) { return InstallDigIni(); }

intptr_t __cdecl AllocateSampleHandle(intptr_t) {
  Lock lock;
  if (g_voices.size() >= 16) return 0;
  g_voices.push_back(std::make_unique<Voice>());
  return reinterpret_cast<intptr_t>(g_voices.back().get());
}

void __cdecl InitSample(intptr_t h) {
  Lock lock;
  if (Voice* v = V(h)) *v = Voice{};
}

void __cdecl SetSampleType(intptr_t h, int format, unsigned flags) {
  Lock lock;
  if (Voice* v = V(h)) { v->format = format; v->flags = int(flags); }
}

void __cdecl SetSampleAddress(intptr_t h, const void* start, uint32_t len) {
  Lock lock;
  if (Voice* v = V(h)) {
    v->addr[0] = static_cast<const uint8_t*>(start);
    v->len[0] = len;
    v->streaming = false;
    v->cur = 0;
  }
}

void __cdecl SetSamplePlaybackRate(intptr_t h, int rate) {
  Lock lock;
  if (Voice* v = V(h)) v->rate = rate > 0 ? rate : 11025;
}

void __cdecl SetSampleVolume(intptr_t h, int vol) {
  Lock lock;
  if (Voice* v = V(h)) v->volume = vol < 0 ? 0 : vol > 127 ? 127 : vol;
}

void __cdecl SetSamplePan(intptr_t h, int pan) {
  Lock lock;
  if (Voice* v = V(h)) v->pan = pan < 0 ? 0 : pan > 127 ? 127 : pan;
}

void __cdecl SetSampleLoopCount(intptr_t h, int n) {
  Lock lock;
  if (Voice* v = V(h)) v->loops = n;
}

void __cdecl StartSample(intptr_t h) {
  Lock lock;
  Voice* v = V(h);
  if (!v) return;
  if (!v->streaming) {
    ResetPlayback(*v);
    v->variant = nullptr;
    v->pitch = v->gain = 1.0f;
    if (SfxNumberOf(v->addr[0]) == kSfxButton) {
      VaryClick(*v);
      static int logged = 0;
      if (logged < 8 && ++logged)
        Log("audio: click %s pitch %.3f gain %.2f", v->variant ? v->variant->name : "button",
            v->pitch, v->gain);
    }
  }
  v->status = kSmpPlaying;
}

void __cdecl StopSample(intptr_t h) {
  Lock lock;
  if (Voice* v = V(h))
    if (v->status == kSmpPlaying) v->status = kSmpStopped;
}

int __cdecl SampleStatus(intptr_t h) {
  Lock lock;
  Voice* v = V(h);
  return v ? v->status : kSmpDone;
}

int __cdecl MinimumSampleBufferSize(intptr_t, int, int) { return 0x2000; }

int __cdecl SampleBufferReady(intptr_t h) {
  Lock lock;
  Voice* v = V(h);
  if (!v) return -1;
  if (!v->streaming) {  // first use after AIL_init_sample
    v->streaming = true;
    v->cur = -1;
    v->next = -1;
    v->fillIdx = -1;
    v->buf[0] = v->buf[1] = kEmpty;
  }
  for (int i = 0; i < 2; ++i) {
    if (v->buf[i] == kEmpty && i != v->cur) {
      v->buf[i] = kReserved;
      v->fillIdx = i;
      return i;
    }
  }
  return -1;
}

void __cdecl LoadSampleBuffer(intptr_t h, int n, const void* start, uint32_t len) {
  Lock lock;
  static unsigned reported = 0;
  if (g_underruns != reported && reported < 20) {
    reported = g_underruns;
    Log("audio: music buffer ran empty (%u times so far)", reported);
  }
  Voice* v = V(h);
  if (!v || n < 0 || n > 1) return;
  v->addr[n] = static_cast<const uint8_t*>(start);
  v->len[n] = len;
  if (v->buf[n] != kQueued) {
    v->buf[n] = kQueued;
    if (v->cur < 0) {
      v->cur = n;
      v->srcPos = 0;
    } else if (v->cur != n) {
      v->next = n;
    }
  }
  if (v->status == kSmpDone) {
    ResetPlayback(*v);
    v->status = kSmpPlaying;
  }
}

}  // namespace

void ApplySettings(const Settings& s) {
  g_musicGain = Gain10(s.musicVolume);
  g_sfxGain = Gain10(s.effectsVolume);
  g_clickMode = int(s.clickSound);
}

void PreviewClick() {
  if (!g_dev || g_clicks.empty()) return;
  Lock lock;
  g_preview = Voice{};
  VaryClick(g_preview);
  if (!g_preview.variant) g_preview.variant = &g_clicks[0];  // "button"
  g_preview.status = kSmpPlaying;
}

bool Install(GameImage& image, const Config& cfg, const Settings& settings) {
  g_image = &image;
  InitializeCriticalSection(&g_cs);
  ApplySettings(settings);

  // Expected bytes: the start of each AIL entry (push regs; mov edx,[call
  // counter]) - the counter address itself is relocated, so it is not compared.
  const HookSpec hooks[] = {
      {"AIL_install_DIG_INI", 0x633B8, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&InstallDigIni},
      {"AIL_install_DIG_driver_file", 0x63496, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&InstallDigDriverFile},
      {"AIL_allocate_sample_handle", 0x635F0, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&AllocateSampleHandle},
      {"AIL_init_sample", 0x6383B, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&InitSample},
      {"AIL_set_sample_address", 0x639AE, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&SetSampleAddress},
      {"AIL_set_sample_type", 0x63A30, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&SetSampleType},
      {"AIL_start_sample", 0x63AB2, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&StartSample},
      {"AIL_stop_sample", 0x63B1F, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&StopSample},
      {"AIL_set_sample_playback_rate", 0x63C66, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&SetSamplePlaybackRate},
      {"AIL_set_sample_volume", 0x63CDC, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&SetSampleVolume},
      {"AIL_set_sample_pan", 0x63D52, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&SetSamplePan},
      {"AIL_set_sample_loop_count", 0x63DC8, {0x57, 0x55, 0x8B, 0x15}, 4, (void*)&SetSampleLoopCount},
      {"AIL_sample_status", 0x63E3E, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&SampleStatus},
      {"AIL_minimum_sample_buffer_size", 0x643BC, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&MinimumSampleBufferSize},
      {"AIL_sample_buffer_ready", 0x644B5, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&SampleBufferReady},
      {"AIL_load_sample_buffer", 0x6459A, {0x56, 0x57, 0x55, 0x8B, 0x15}, 5, (void*)&LoadSampleBuffer},
  };
  for (const HookSpec& h : hooks)
    if (!VerifyHook(image, h)) {
      Log("audio: library code not as expected - sound disabled");
      return false;
    }
  for (const HookSpec& h : hooks) InstallHook(image, h);

  // Click sounds for "Mixed": the game's click plus its unused ones, cut to
  // a short length and matched in loudness to the original click.
  const char* names[] = {"button", "click08", "switch02", "button02", "switch01"};
  for (const char* n : names) {
    Variant v;
    if (LoadClick(cfg, n, 170.0f, &v)) g_clicks.push_back(std::move(v));
  }
  if (!g_clicks.empty() && !std::strcmp(g_clicks[0].name, "button")) {
    float target = Rms(g_clicks[0].data);
    for (auto& c : g_clicks) {
      float r = Rms(c.data);
      if (r > 0)
        for (float& x : c.data) x *= target / r;
    }
  }
  Log("audio: %zu click sounds for \"Mixed\"", g_clicks.size());

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    Log("audio: SDL audio unavailable (%s) - no sound", SDL_GetError());
    return true;
  }
  SDL_AudioSpec want{}, have{};
  want.freq = 48000;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = AudioCallback;
  g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!g_dev) {
    Log("audio: no audio device (%s) - no sound", SDL_GetError());
    return true;
  }
  g_devRate = have.freq;
  Log("audio: %s, %d Hz, %d-frame buffer", SDL_GetCurrentAudioDriver(), have.freq, have.samples);
  SDL_PauseAudioDevice(g_dev, 0);
  return true;
}

}  // namespace asc::audio
