// Test-only DYLD helper injected into the ISOLATED COPIED candidate exe.
// Command-file driven: every 250 ms the worker polls LHT_CMD for a line:
//   resize <w>x<h>    -> NSWindow setContentSize on the main queue (real
//                        drawable change in the running process)
//   key/kdown/kup <kc> -> SDL_KeyboardEvent push (real SDL event)
//   seq <ms> kc,...   -> spaced key pushes on a global queue
// Own-process mechanisms only. Key delivery uses dlsym'd SDL_PushEvent
// (measured: NSEvent postEvent and CGEventPostToPid never reach the SDL
// queue for this unfocused window). No production symbols are patched.
#import <AppKit/AppKit.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *L;
static char CMD[1024];
static long long consumed; // cmd-file lines already executed

#define LOG(...)                                                               \
  do {                                                                         \
    if (L) {                                                                   \
      fprintf(L, __VA_ARGS__);                                                 \
      fflush(L);                                                               \
    }                                                                          \
    fprintf(stderr, "[livehost] " __VA_ARGS__);                                \
  } while (0)

// SDL3 event constants from the vendored include/SDL3 headers (version is
// pinned by the build). SDL_KeyboardEvent layout:
//   u32 type @0, u32 reserved @4, u64 timestamp @8, u32 windowID @16,
//   u32 keyboardID @20, u32 scancode @24, u32 key @28, u16 mod @32,
//   u16 raw @34, u8 down @36, u8 repeat @37  (union slot = 128 bytes).
#define EVT_KEY_DOWN 0x300u
#define EVT_KEY_UP 0x301u
#define SCANCODE_F12 69u
#define SCANCODE_LEFT 80u
#define SCANCODE_RETURN 40u
#define SCANCODE_ESCAPE 41u
#define KEYCODE_MASK 0x40000000u

// macOS keycode -> (SDL scancode, SDL keycode)
static const struct {
  unsigned kc, scan, key;
} KEYS[] = {
    {111, SCANCODE_F12, KEYCODE_MASK | 69u},  // kVK_F12 -> SDLK_F12
    {123, SCANCODE_LEFT, KEYCODE_MASK | 80u}, // kVK_Left -> SDLK_LEFT
    {36, SCANCODE_RETURN, 0x0du},             // kVK_Return -> SDLK_RETURN
    {53, SCANCODE_ESCAPE, 0x1bu},             // kVK_Escape -> SDLK_ESCAPE
};

static void push_sdl_key(unsigned scan, unsigned key, int down,
                         unsigned window) {
  typedef int (*push_fn)(const void *);
  static push_fn Push;
  static int tried;
  if (!tried) {
    Push = (push_fn)dlsym(RTLD_DEFAULT, "SDL_PushEvent");
    if (!Push)
      Push = (push_fn)dlsym(RTLD_DEFAULT, "SDL_PushEvent_REAL");
    tried = 1;
  }
  if (!Push)
    return;
  unsigned char ev[128];
  memset(ev, 0, sizeof(ev));
  *(unsigned *)(ev + 0) = down ? EVT_KEY_DOWN : EVT_KEY_UP;
  *(unsigned *)(ev + 16) = window;
  *(unsigned *)(ev + 24) = scan;
  *(unsigned *)(ev + 28) = key;
  ev[36] = down ? 1 : 0;
  Push(ev);
}

static unsigned focused_window_id(void) {
  typedef void *(*focus_fn)(void);
  typedef unsigned (*wid_fn)(void *);
  focus_fn Focus = (focus_fn)dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus");
  if (!Focus)
    Focus = (focus_fn)dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus_REAL");
  wid_fn Wid = (wid_fn)dlsym(RTLD_DEFAULT, "SDL_GetWindowID");
  if (!Wid)
    Wid = (wid_fn)dlsym(RTLD_DEFAULT, "SDL_GetWindowID_REAL");
  if (Focus && Wid) {
    void *w = Focus();
    if (w)
      return Wid(w);
  }
  return 0;
}

static void post_key(unsigned short kc, NSUInteger mods, int phase) {
  unsigned scan = 0, key = 0;
  for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
    if (KEYS[i].kc == kc) {
      scan = KEYS[i].scan;
      key = KEYS[i].key;
    }
  unsigned window = focused_window_id();
  int pushed = 0;
  if (scan) {
    for (int ph = 0; ph < 2; ph++) {
      if (phase != 0 && ph != phase - 1)
        continue;
      push_sdl_key(scan, key, ph == 0, window);
    }
    pushed = 1;
  }
  LOG("posted keycode %u mods %lu phase %d (sdl_push=%d win=%u)\n",
      (unsigned)kc, (unsigned long)mods, phase, pushed, window);
}

static void do_resize(int w, int h) {
  for (NSWindow *win in NSApp.windows) {
    if (!win.contentView)
      continue;
    [win setContentSize:NSMakeSize(w, h)];
    LOG("resize -> content %.0fx%.0f frame %.0fx%.0f\n",
        win.contentView.frame.size.width, win.contentView.frame.size.height,
        win.frame.size.width, win.frame.size.height);
  }
}

static void exec_line(char *line) {
  if (!strncmp(line, "resize ", 7)) {
    int w = 0, h = 0;
    if (sscanf(line + 7, "%dx%d", &w, &h) == 2)
      do_resize(w, h);
  } else if (!strncmp(line, "key ", 4)) {
    unsigned kc = 0, mods = 0;
    sscanf(line + 4, "%u %u", &kc, &mods);
    post_key((unsigned short)kc, mods, 0);
  } else if (!strncmp(line, "kdown ", 6)) {
    unsigned kc = 0;
    sscanf(line + 6, "%u", &kc);
    post_key((unsigned short)kc, 0, 1);
  } else if (!strncmp(line, "kup ", 4)) {
    unsigned kc = 0;
    sscanf(line + 4, "%u", &kc);
    post_key((unsigned short)kc, 0, 2);
  } else if (!strncmp(line, "seq ", 4)) {
    int ms = 0, used = 0;
    if (sscanf(line + 4, "%d%n", &ms, &used) < 1)
      return;
    unsigned *kcs = malloc(sizeof(unsigned) * 32);
    int n = 0;
    char *p = strtok(line + 4 + used, ",");
    while (p && n < 32) {
      kcs[n++] = (unsigned)strtoul(p, NULL, 0);
      p = strtok(NULL, ",");
    }
    // Post from a global queue: SDL_PushEvent is thread-safe, and the seq
    // must not depend on the main queue draining (the modal rewind loop
    // runs its own event pump on the main thread).
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      for (int i = 0; i < n; i++) {
        usleep((useconds_t)(ms * 1000));
        post_key((unsigned short)kcs[i], 0, 0);
      }
      free(kcs);
    });
  }
}

static void poll_cmd(void) {
  FILE *f = fopen(CMD, "r");
  if (f) {
    char buf[2048];
    long long skip = consumed;
    while (skip-- > 0) {
      if (!fgets(buf, sizeof(buf), f))
        break;
    }
    while (fgets(buf, sizeof(buf), f)) {
      consumed++;
      char *t = buf + strlen(buf) - 1;
      while (t >= buf && (*t == '\n' || *t == ' '))
        *t-- = 0;
      if (!*buf)
        continue;
      LOG("cmd: %s\n", buf);
      char *line = strdup(buf);
      dispatch_async(dispatch_get_main_queue(), ^{
        exec_line(line);
        free(line);
      });
    }
    fclose(f);
  }
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, (int64_t)(250 * NSEC_PER_MSEC)),
      dispatch_get_main_queue(), ^{
        poll_cmd();
      });
}

__attribute__((constructor)) static void lht_boot(void) {
  const char *lg = getenv("LHT_LOG");
  L = fopen(lg ? lg : "/tmp/livehost.log", "w");
  const char *c = getenv("LHT_CMD");
  snprintf(CMD, sizeof(CMD), "%s", c ? c : "/tmp/livehost.cmd");
  if (L)
    setvbuf(L, NULL, _IONBF, 0);
  LOG("injected pid=%d cmd=%s\n", getpid(), CMD);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   poll_cmd();
                 });
}
