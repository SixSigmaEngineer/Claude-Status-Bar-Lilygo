/*
 * Claude Status Bar — LilyGo T-Display S3 Long (ESP32-S3, 640x180 AMOLED, touch)
 *
 * Shows live Claude session status (model, current tool, elapsed, tokens,
 * context %) plus a usage-limits page. Fed by claude_bar_bridge.py on the PC
 * over USB serial @ 115200 as newline-delimited JSON.
 *
 * Controls (with a working touch chip - see touch_drv.h):
 *   - Tap screen            : switch page (status <-> usage)
 *   - Swipe left/right      : cycle active session
 *   - Touch-hold            : unbound (reserved for future use)
 *   - BOOT button short     : cycle active session
 *   - BOOT button long (1s) : flip display 180° (saved to flash)
 * Without one (blind-tap fallback): tap = switch page, BOOT as above.
 *
 * Build notes: requires AXS15231B.cpp/.h + pins_config.h from the official
 * LilyGo T-Display-S3-Long repo copied into this sketch folder (the
 * build_and_flash.ps1 script does this automatically), plus libraries
 * "Adafruit GFX Library" and "ArduinoJson".
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "AXS15231B.h"
#include "pins_config.h"
#include "touch_drv.h"

/* ---------- fallbacks in case pins_config.h names differ ---------- */
#ifndef TOUCH_IICSCL
#define TOUCH_IICSCL 10
#endif
#ifndef TOUCH_IICSDA
#define TOUCH_IICSDA 15
#endif
#ifndef TOUCH_RES
#define TOUCH_RES 16
#endif
#ifndef TOUCH_INT
#define TOUCH_INT 11
#endif

#define CANVAS_W 640
#define CANVAS_H 180
#define PANEL_W  180
#define PANEL_H  640

/* If colors look wrong (blue/orange swapped), set to 0 and re-run the build script. */
#define SWAP_BYTES 1

/* ---------- colors (RGB565) ---------- */
#define C_BG     0x0000
#define C_PANEL  0x18E4
#define C_TEXT   0xEF5C
#define C_DIM    0x8C72
#define C_ORANGE 0xCB48
#define C_GREEN  0x3EEF
#define C_YELLOW 0xF647
#define C_RED    0xEA88
#define C_BAR_BG 0x2124

#define BOOT_BTN 0

/* logo (streamed from the bridge, persisted in flash) */
#define LOGO_W 48
#define LOGO_H 48
#define LOGO_BYTES (LOGO_W * LOGO_H * 2)

/* ---------- state ---------- */
struct Sess {
  char pj[22];  // project (cwd basename)
  char nm[40];  // session title (custom > ai-title > summary)
  char md[22];  // model, e.g. "Fable 5"
  char st[10];  // run | tool | wait | idle | done
  char tl[22];  // tool name
  char td[34];  // tool detail ("npm test", "auth.ts", question text)
  char ef[12];  // effort
  char tk[10];  // context tokens, human ("612k")
  int  sa;      // active subagents
  long el;      // elapsed seconds (at packet time)
  long ti, to;  // tokens in / out
  int  cx;      // context %
  bool at;      // needs attention
};

#define MAX_SES 8
Sess ses[MAX_SES];
int  nSes = 0, act = 0;

struct { int p5 = -1, p7 = -1; char r5[14] = ""; char r7[14] = ""; bool est = true; } usage;

GFXcanvas16 *cv = nullptr;
uint16_t    *pfb = nullptr;      // portrait framebuffer (180x640), PSRAM
uint16_t    *logoBuf = nullptr;
bool         hasLogo = false;

// driver internals (AXS15231B.cpp) — used to pump its async transfer queue,
// exactly like LilyGo's own TFT example does
extern uint32_t transfer_num;
extern size_t   lcd_PushColors_len;
Preferences  prefs;

uint32_t lastPacket = 0;
int      page = 0;          // 0 = status, 1 = usage
bool     flipped = false;
bool     dirty = true;
uint32_t lastDraw = 0;
uint8_t  spinPhase = 0;

/* touch/button gesture state */
volatile bool touchIRQ = false;
volatile uint32_t irqCount = 0;
void IRAM_ATTR touchISR() { touchIRQ = true; irqCount++; }
TouchBackend touchBE = TOUCH_NONE;
bool     touching = false;
int16_t  tX0 = 0, tY0 = 0, tXl = 0, tYl = 0;
uint32_t tDownAt = 0;
uint32_t btnDownAt = 0;
bool     btnWas = false;

static char lineBuf[4096];
static size_t lineLen = 0;

/* =========================== helpers =========================== */

static void fmtK(long v, char *out, size_t n) {
  if (v >= 1000000L)      snprintf(out, n, "%.1fM", v / 1000000.0);
  else if (v >= 1000L)    snprintf(out, n, "%.1fk", v / 1000.0);
  else                    snprintf(out, n, "%ld", v);
}

static void fmtElapsed(long s, char *out, size_t n) {
  if (s < 0) s = 0;
  if (s < 180)            snprintf(out, n, "%lds", s);
  else if (s < 3600)      snprintf(out, n, "%ldm%02lds", s / 60, s % 60);
  else                    snprintf(out, n, "%ldh%02ldm", s / 3600, (s % 3600) / 60);
}

static int16_t textW(const char *s) {
  int16_t x1, y1; uint16_t w, h;
  cv->getTextBounds(s, 0, 100, &x1, &y1, &w, &h);
  return (int16_t)w;
}

static void drawCentered(const char *s, int cx0, int cx1, int baseY, uint16_t color) {
  int16_t w = textW(s);
  int x = cx0 + ((cx1 - cx0) - w) / 2;
  if (x < cx0) x = cx0;
  cv->setTextColor(color);
  cv->setCursor(x, baseY);
  cv->print(s);
}

static uint16_t ctxColor(int pct) {
  if (pct >= 80) return C_RED;
  if (pct >= 50) return C_YELLOW;
  return C_GREEN;
}

/* Push the 640x180 landscape canvas to the panel.
 * The panel is addressed PORTRAIT (180 wide x 640 tall). We transpose the
 * whole canvas into a portrait framebuffer, then push it the way LilyGo's
 * TFT example does: one full-frame call, then pump the driver's async
 * queue with NULL calls until the frame has fully drained. */
static void pushCanvas() {
  const uint16_t *src = cv->getBuffer();
  uint16_t *dst = pfb;
  for (int yp = 0; yp < PANEL_H; yp++) {
    if (!flipped) {
      // panel(xp,yp) <- canvas(xl=yp, yl=179-xp)
      const uint16_t *base = src + yp;
      for (int xp = 0; xp < PANEL_W; xp++) {
        uint16_t v = base[(uint32_t)(PANEL_W - 1 - xp) * CANVAS_W];
#if SWAP_BYTES
        v = (v >> 8) | (v << 8);
#endif
        *dst++ = v;
      }
    } else {
      // panel(xp,yp) <- canvas(xl=639-yp, yl=xp)
      const uint16_t *base = src + (CANVAS_W - 1 - yp);
      for (int xp = 0; xp < PANEL_W; xp++) {
        uint16_t v = base[(uint32_t)xp * CANVAS_W];
#if SWAP_BYTES
        v = (v >> 8) | (v << 8);
#endif
        *dst++ = v;
      }
    }
  }

  lcd_PushColors(0, 0, PANEL_W, PANEL_H, pfb);
  uint32_t t0 = millis();
  while (lcd_PushColors_len > 0 || transfer_num > 0) {
    if (transfer_num <= 1 && lcd_PushColors_len > 0) {
      lcd_PushColors(0, 0, 0, 0, NULL);   // pump remaining chunks
    } else {
      delayMicroseconds(100);
    }
    if (millis() - t0 > 500) break;       // safety timeout
  }
}

/* =========================== drawing =========================== */

static void drawSpinner(int cx, int cy, uint16_t color) {
  // pulsing 4-point star (Claude-ish sparkle)
  static const int sizes[6] = { 8, 11, 14, 16, 14, 11 };
  int r = sizes[spinPhase % 6];
  cv->fillTriangle(cx - r, cy, cx, cy - 4, cx, cy + 4, color);
  cv->fillTriangle(cx + r, cy, cx, cy - 4, cx, cy + 4, color);
  cv->fillTriangle(cx, cy - r, cx - 4, cy, cx + 4, cy, color);
  cv->fillTriangle(cx, cy + r, cx - 4, cy, cx + 4, cy, color);
}

static void drawUpTri(int x, int y, uint16_t c)   { cv->fillTriangle(x, y + 8, x + 8, y + 8, x + 4, y, c); }
static void drawDownTri(int x, int y, uint16_t c) { cv->fillTriangle(x, y, x + 8, y, x + 4, y + 8, c); }

static void drawHeader() {
  // page dots, session indicator, attention alert
  char buf[24];
  cv->setFont(NULL);  // built-in 6x8
  cv->setTextSize(1);

  // attention banner (any session waiting)
  bool anyAttn = false; int attnIdx = -1;
  for (int i = 0; i < nSes; i++) if (ses[i].at && i != act) { anyAttn = true; attnIdx = i; break; }
  if (anyAttn) {
    cv->fillRect(0, 0, CANVAS_W, 14, C_ORANGE);
    snprintf(buf, sizeof(buf), "! session %c waiting", 'A' + attnIdx);
    cv->setTextColor(C_BG);
    cv->setCursor(6, 3);
    cv->print(buf);
  }

  // session letter + count, top right
  if (nSes > 0) {
    snprintf(buf, sizeof(buf), "%c %d/%d", 'A' + act, act + 1, nSes);
    cv->setTextColor(anyAttn ? C_BG : C_DIM);
    cv->setCursor(CANVAS_W - 6 * strlen(buf) - 34, anyAttn ? 3 : 6);
    cv->print(buf);
  }
  // page dots
  int dx = CANVAS_W - 24;
  int dy = anyAttn ? 6 : 9;
  cv->fillCircle(dx,      dy, 3, page == 0 ? C_ORANGE : C_DIM);
  cv->fillCircle(dx + 12, dy, 3, page == 1 ? C_ORANGE : C_DIM);
}

static void drawMessage(const char *title, const char *sub) {
  cv->fillScreen(C_BG);
  cv->setFont(&FreeSansBold18pt7b);
  drawCentered(title, 0, CANVAS_W, 92, C_TEXT);
  cv->setFont(&FreeSans9pt7b);
  drawCentered(sub, 0, CANVAS_W, 130, C_DIM);
  drawHeader();
}

static void drawStatusPage() {
  cv->fillScreen(C_BG);

  if (nSes == 0) {
    drawMessage("No sessions", "waiting for Claude activity...");
    return;
  }
  if (act >= nSes) act = 0;
  Sess &s = ses[act];

  /* ----- left column: logo + context ----- */
  if (hasLogo && logoBuf) {
    cv->drawRGBBitmap(14, 12, logoBuf, LOGO_W, LOGO_H);
  } else {
    // no logo uploaded: small wordmark placeholder
    cv->setFont(&FreeSansBold12pt7b);
    cv->setTextColor(C_ORANGE);
    cv->setCursor(12, 40);
    cv->print("**");
  }

  cv->setFont(NULL); cv->setTextSize(1);
  cv->setTextColor(C_DIM);
  cv->setCursor(10, 112);
  cv->print("CONTEXT");

  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", s.cx);
  cv->setFont(&FreeSansBold18pt7b);
  cv->setTextColor(ctxColor(s.cx));
  cv->setCursor(10, 158);
  cv->print(pct);

  // raw context tokens under the % (so the % has a visible denominator story)
  if (s.tk[0]) {
    cv->setFont(NULL); cv->setTextSize(1);
    cv->setTextColor(C_DIM);
    cv->setCursor(10, 168);
    cv->print(s.tk);
  }

  cv->drawFastVLine(178, 18, 148, C_PANEL);

  int zone0 = 196, zone1 = CANVAS_W - 10;

  /* ----- row 1: PROJECT (the thing that ties display to session) ----- */
  {
    const char *pj = s.pj[0] ? s.pj : (s.nm[0] ? s.nm : "Claude");
    cv->setFont(&FreeSansBold12pt7b);
    cv->setTextColor(C_TEXT);
    cv->setCursor(zone0, 40);
    cv->print(pj);
    // model · effort, right-aligned on the same row
    char mline[36];
    if (s.ef[0]) snprintf(mline, sizeof(mline), "%s · %s", s.md, s.ef);
    else         strlcpy(mline, s.md, sizeof(mline));
    cv->setFont(&FreeSans9pt7b);
    int16_t mw = textW(mline);
    cv->setTextColor(C_DIM);
    cv->setCursor(zone1 - mw, 38);
    cv->print(mline);
  }

  /* ----- row 2: session title (ai-title) ----- */
  if (s.pj[0] && s.nm[0]) {
    cv->setFont(&FreeSans9pt7b);
    cv->setTextColor(C_DIM);
    cv->setCursor(zone0, 64);
    cv->print(s.nm);
  }

  /* ----- row 3: state word (real state, no clodisms) ----- */
  const char *word;
  uint16_t wcol = C_TEXT;
  bool animate = false;
  if      (!strcmp(s.st, "tool")) { word = s.tl[0] ? s.tl : "Tool"; animate = true; }
  else if (!strcmp(s.st, "run"))  { word = "Running"; animate = true; }
  else if (!strcmp(s.st, "wait")) { word = "Waiting on you"; wcol = C_ORANGE; }
  else if (!strcmp(s.st, "done")) { word = "Done"; wcol = C_GREEN; }
  else                            { word = "Idle"; wcol = C_DIM; }

  cv->setFont(&FreeSansBold24pt7b);
  int16_t w = textW(word);
  if (w > (zone1 - zone0) - 70) {          // too wide: drop to a smaller font
    cv->setFont(&FreeSansBold18pt7b);
    w = textW(word);
  }
  int tx = zone0 + 34;
  int baseY = 110;
  if (animate) {
    drawSpinner(tx - 26, baseY - 13, C_ORANGE);
  } else if (!strcmp(s.st, "wait")) {
    cv->drawCircle(tx - 26, baseY - 13, 9, C_ORANGE);
    cv->drawCircle(tx - 26, baseY - 13, 8, C_ORANGE);
  }
  cv->setTextColor(wcol);
  cv->setCursor(tx, baseY);
  cv->print(word);

  /* ----- row 4: detail - what it's doing / waiting for, + subagents ----- */
  {
    char dline[64] = "";
    if (!strcmp(s.st, "wait") && s.tl[0] && strcmp(s.tl, "Question"))
      snprintf(dline, sizeof(dline), "approve: %s%s%s", s.tl,
               s.td[0] ? " · " : "", s.td);
    else if (s.td[0])
      strlcpy(dline, s.td, sizeof(dline));
    if (s.sa > 0) {
      char sab[20];
      snprintf(sab, sizeof(sab), "%s%d subagent%s", dline[0] ? " · " : "",
               s.sa, s.sa > 1 ? "s" : "");
      strlcat(dline, sab, sizeof(dline));
    }
    if (dline[0]) {
      cv->setFont(&FreeSans9pt7b);
      cv->setTextColor(C_DIM);
      cv->setCursor(zone0 + 8, 137);
      cv->print(dline);
    }
  }

  /* ----- row 5: elapsed + tokens ----- */
  long elShown = s.el;
  if (animate) elShown += (long)((millis() - lastPacket) / 1000);
  char eb[12], tib[12], tob[12];
  fmtElapsed(elShown, eb, sizeof(eb));
  fmtK(s.ti, tib, sizeof(tib));
  fmtK(s.to, tob, sizeof(tob));

  cv->setFont(&FreeSans9pt7b);
  cv->setTextColor(C_DIM);
  int gap = 22, icon = 12;
  int x = zone0;
  int yBase = 166;
  cv->setCursor(x, yBase); cv->print(eb); x += textW(eb) + gap;
  drawUpTri(x, yBase - 9, C_GREEN); x += icon;
  cv->setCursor(x, yBase); cv->print(tib); x += textW(tib) + gap;
  drawDownTri(x, yBase - 9, C_ORANGE); x += icon;
  cv->setCursor(x, yBase); cv->print(tob);

  drawHeader();
}

static void drawBar(int y, const char *label, int pct, const char *resets) {
  cv->setFont(&FreeSans9pt7b);
  cv->setTextColor(C_DIM);
  cv->setCursor(12, y + 20);
  cv->print(label);

  int bx = 120, bw = 380, bh = 26;
  cv->fillRoundRect(bx, y, bw, bh, 4, C_BAR_BG);
  if (pct >= 0) {
    int fw = (int)((long)bw * (pct > 100 ? 100 : pct) / 100);
    uint16_t c = pct >= 90 ? C_RED : (pct >= 70 ? C_YELLOW : C_GREEN);
    if (fw > 2) cv->fillRoundRect(bx, y, fw, bh, 4, c);
    char p[8]; snprintf(p, sizeof(p), "%d%%", pct);
    cv->setFont(&FreeSansBold12pt7b);
    cv->setTextColor(C_TEXT);
    cv->setCursor(bx + bw + 14, y + 21);
    cv->print(p);
  } else {
    cv->setFont(&FreeSansBold12pt7b);
    cv->setTextColor(C_DIM);
    cv->setCursor(bx + bw + 14, y + 21);
    cv->print("--");
  }
  cv->setFont(NULL); cv->setTextSize(1);
  cv->setTextColor(C_DIM);
  cv->setCursor(bx, y + bh + 6);
  char rb[32];
  snprintf(rb, sizeof(rb), "resets in %s", resets[0] ? resets : "?");
  cv->print(rb);
}

static void drawUsagePage() {
  cv->fillScreen(C_BG);
  cv->setFont(NULL); cv->setTextSize(1);
  cv->setTextColor(C_ORANGE);
  cv->setCursor(12, 8);
  cv->print(usage.est ? "CLAUDE USAGE (estimated)" : "CLAUDE USAGE");

  drawBar(38,  "5-HOUR", usage.p5, usage.r5);
  drawBar(112, "7-DAY",  usage.p7, usage.r7);
  drawHeader();
}

static void redraw() {
  if (millis() - lastPacket > 10000 && lastPacket != 0) {
    drawMessage("Bridge offline", "start claude_bar_bridge on your PC");
  } else if (lastPacket == 0) {
    drawMessage("Claude Status Bar", "connect USB + run bridge on PC");
  } else if (page == 1) {
    drawUsagePage();
  } else {
    drawStatusPage();
  }
  pushCanvas();
}

/* =========================== input =========================== */

static void applyTap()        { page = (page + 1) % 2; dirty = true; }
static void applyCycle(int d) { if (nSes > 1) act = (act + nSes + d) % nSes; dirty = true; }
static void applyDoubleTap()  { applyCycle(+1); }
static void applyLongPress()  { flipped = !flipped; prefs.putBool("flip", flipped); dirty = true; }

/* Gesture engine over real coordinates (CST3530 / AXS backends).
 * The controller reports in portrait axes: x = short side (0..179),
 * y = long side (0..639). In landscape, a horizontal swipe is a y-run. */
static void gestureFrom(const TouchSample &s) {
  uint32_t now = millis();
  if (s.down && !touching) {                 // finger down
    touching = true;
    tX0 = tXl = s.x; tY0 = tYl = s.y;
    tDownAt = now;
  } else if (s.down && touching) {           // drag
    tXl = s.x; tYl = s.y;
  } else if (!s.down && touching) {          // finger up
    touching = false;
    // the lift report itself carries coordinates - use them: redraws block
    // the poll loop long enough that drag samples are often missed entirely
    if (s.x || s.y) { tXl = s.x; tYl = s.y; }
    int dy = tYl - tY0;                      // landscape-horizontal travel
    uint32_t dt = now - tDownAt;
    if (abs(dy) >= 80 && dt < 900) {
      int dir = (dy < 0) ? +1 : -1;          // swipe toward connector = next
      if (flipped) dir = -dir;
      Serial.printf("[touch] swipe %+d (dy=%d dt=%lu) -> session\n",
                    dir, dy, (unsigned long)dt);
      applyCycle(dir);
    } else if (dt < 350 && abs(dy) < 40) {
      Serial.printf("[touch] tap (dy=%d dt=%lu) -> page\n",
                    dy, (unsigned long)dt);
      applyTap();
    }
    // touch-hold (no swipe) is deliberately unbound - reserved
  }
}

static void pollTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20) return;
  lastPoll = millis();

  if (touchBE != TOUCH_NONE) {
    TouchSample s = touchRead(touchBE);
    if (s.valid) gestureFrom(s);
    return;
  }

  // Fallback (no touch chip detected): blind INT-pulse tap. After any touch
  // the chip pulses INT for a while, so wait for quiet before re-arming.
  static uint32_t lastIrqSeen = 0, lastActivity = 0;
  static bool armed = true;
  uint32_t now = millis();
  uint32_t ic = irqCount;
  if (ic != lastIrqSeen) {
    lastIrqSeen = ic;
    lastActivity = now;
    if (armed) {
      armed = false;
      Serial.println("[touch] tap -> page");
      applyTap();
    }
  }
  if (!armed && now - lastActivity > 1200) {
    armed = true;
    Serial.println("[touch] re-armed");
  }
}

static void pollButton() {
  bool down = digitalRead(BOOT_BTN) == LOW;
  uint32_t now = millis();
  if (down && !btnWas) btnDownAt = now;
  if (!down && btnWas) {
    if (now - btnDownAt > 800) applyLongPress();  // long = flip display
    else applyDoubleTap();                        // short = cycle session
  }
  btnWas = down;
}

/* =========================== serial =========================== */

static void handleLine(const char *line) {
  static StaticJsonDocument<14336> doc;  // static: keep off the loop task stack
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.printf("[rx] parse error: %s (len %u)\n", err.c_str(), (unsigned)strlen(line));
    return;
  }
  Serial.printf("[rx] ok t=%s\n", (const char *)(doc["t"] | "?"));
  const char *t = doc["t"] | "";
  if (!strcmp(t, "s")) {
    JsonArray arr = doc["ses"].as<JsonArray>();
    int n = 0;
    for (JsonObject o : arr) {
      if (n >= MAX_SES) break;
      Sess &s = ses[n];
      strlcpy(s.pj, o["pj"] | "",      sizeof(s.pj));
      strlcpy(s.nm, o["nm"] | "",      sizeof(s.nm));
      strlcpy(s.md, o["md"] | "Claude", sizeof(s.md));
      strlcpy(s.st, o["st"] | "idle",  sizeof(s.st));
      strlcpy(s.tl, o["tl"] | "",      sizeof(s.tl));
      strlcpy(s.td, o["td"] | "",      sizeof(s.td));
      strlcpy(s.ef, o["ef"] | "",      sizeof(s.ef));
      strlcpy(s.tk, o["tk"] | "",      sizeof(s.tk));
      s.sa = o["sa"] | 0;
      s.el = o["el"] | 0L;
      s.ti = o["ti"] | 0L;
      s.to = o["to"] | 0L;
      s.cx = o["cx"] | 0;
      s.at = o["at"] | false;
      n++;
    }
    nSes = n;
    // Follow the bridge's session pick only when it CHANGES (a real
    // auto-follow event, e.g. a session starts waiting). Otherwise the
    // 1/sec packets would stomp a manual BOOT-button selection.
    static int lastPktAct = -1;
    int a = doc["act"] | 0;
    if (a != lastPktAct && a >= 0 && a < nSes) act = a;
    lastPktAct = a;
    if (doc.containsKey("us")) {
      JsonObject u = doc["us"];
      usage.p5 = u["p5"] | -1;
      usage.p7 = u["p7"] | -1;
      usage.est = u["est"] | true;
      strlcpy(usage.r5, u["r5"] | "", sizeof(usage.r5));
      strlcpy(usage.r7, u["r7"] | "", sizeof(usage.r7));
    }
    lastPacket = millis();
    dirty = true;
  } else if (!strcmp(t, "ping")) {
    lastPacket = millis();
  } else if (!strcmp(t, "lg")) {
    // logo chunk: {"t":"lg","off":<byte offset>,"px":"<hex>","last":bool}
    if (!logoBuf) return;
    long off = doc["off"] | -1L;
    const char *hex = doc["px"] | "";
    size_t hlen = strlen(hex);
    if (off < 0 || hlen % 2 || off + (long)(hlen / 2) > LOGO_BYTES) return;
    uint8_t *dst = (uint8_t *)logoBuf + off;
    for (size_t i = 0; i < hlen; i += 2) {
      auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
      };
      dst[i / 2] = (nib(hex[i]) << 4) | nib(hex[i + 1]);
    }
    if (doc["last"] | false) {
      prefs.putBytes("logo", logoBuf, LOGO_BYTES);
      hasLogo = true;
      dirty = true;
      Serial.println("[logo] received + saved");
    }
  } else if (!strcmp(t, "lgclr")) {
    prefs.remove("logo");
    hasLogo = false;
    dirty = true;
    Serial.println("[logo] cleared");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      if (lineLen > 0) handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;  // overflow — discard
    }
  }
}

/* =========================== setup/loop =========================== */

void setup() {
  Serial.setRxBufferSize(16384);   // default 256B overflows during redraws
  Serial.begin(115200);

  prefs.begin("cbar", false);
  flipped = prefs.getBool("flip", false);

  pinMode(BOOT_BTN, INPUT_PULLUP);

  // init order per LilyGo's GFX_AXS15231B_Image example (2025):
  // touch reset pulse (100ms low), I2C up, PMU config, then display init
  pinMode(TOUCH_RES, OUTPUT);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(TOUCH_INT, touchISR, FALLING);
  digitalWrite(TOUCH_RES, HIGH); delay(2);
  digitalWrite(TOUCH_RES, LOW);  delay(100);
  digitalWrite(TOUCH_RES, HIGH); delay(2);
  Wire.setBufferSize(1100);   // CST3530 fw upload sends 1026-byte writes
  Wire.begin(TOUCH_IICSDA, TOUCH_IICSCL);
  Wire.setClock(200000);      // hyn driver runs the bus at 200 kHz

  // SY6970 battery-charger config (required for stable power w/o battery):
  // disable ILIM pin + max input current limit; turn off BATFET
  Wire.beginTransmission(0x6A); Wire.write(0x00); Wire.write(0x3F); Wire.endTransmission();
  Wire.beginTransmission(0x6A); Wire.write(0x09); Wire.write(0x64); Wire.endTransmission();

  // display
  axs15231_init();

  // backlight ON (the panel is dark without this)
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif

  // frame buffers (land in PSRAM)
  cv = new GFXcanvas16(CANVAS_W, CANVAS_H);
  pfb = (uint16_t *)heap_caps_malloc((size_t)PANEL_W * PANEL_H * 2,
                                     MALLOC_CAP_SPIRAM);
  if (!pfb) pfb = (uint16_t *)malloc((size_t)PANEL_W * PANEL_H * 2);
  Serial.printf("[boot] canvas=%p pfb=%p psram=%u\n",
                (void *)(cv ? cv->getBuffer() : nullptr),
                (void *)pfb, (unsigned)ESP.getPsramSize());
  if (!cv || !cv->getBuffer() || !pfb) {
    Serial.println("[boot] buffer alloc FAILED");
    while (true) delay(1000);
  }

  // boot splash: solid orange so we know the display path works
  Serial.println("[boot] pushing splash...");
  cv->fillScreen(C_ORANGE);
  pushCanvas();
  Serial.println("[boot] splash pushed OK");
  delay(600);

  // touch: detect which revision's controller we have and bring it up.
  // Done after display init: the AXS integrated touch (old revision) only
  // answers once the panel is initialized, and the CST path pulses its own
  // reset (GPIO2), which is safe on old boards where that pin is unused.
  touchBE = touchDetect();
  if (touchBE == TOUCH_CST) {
    Serial.println("[touch] CST3530 detected (new hw revision)");
    if (!cstInit()) {
      Serial.println("[touch] CST init failed - falling back to blind-tap");
      touchBE = TOUCH_NONE;
    }
  } else if (touchBE == TOUCH_AXS) {
    Serial.println("[touch] AXS15231B integrated touch detected (old hw revision)");
  } else {
    Serial.println("[touch] no touch controller answered - blind-tap fallback");
  }

  // load saved logo, if one was uploaded
  logoBuf = (uint16_t *)malloc(LOGO_BYTES);
  if (logoBuf && prefs.getBytesLength("logo") == LOGO_BYTES) {
    prefs.getBytes("logo", logoBuf, LOGO_BYTES);
    hasLogo = true;
    Serial.println("[logo] loaded from flash");
  }

  redraw();
}

void loop() {
  pollSerial();
  pollTouch();
  pollButton();

  uint32_t now = millis();
  bool animating = false;
  if (nSes > 0 && page == 0) {
    const char *st = ses[act].st;
    animating = (!strcmp(st, "run") || !strcmp(st, "tool"));
  }
  uint32_t interval = animating ? 150 : 500;
  if (dirty || now - lastDraw >= interval) {
    spinPhase++;
    redraw();
    dirty = false;
    lastDraw = now;
  }

  static uint32_t lastBeat = 0;
  if (now - lastBeat > 3000) {
    lastBeat = now;
    Serial.printf("[beat] up=%lus heap=%u ses=%d page=%d int=%d irq=%lu\n",
                  (unsigned long)(now / 1000), (unsigned)ESP.getFreeHeap(),
                  nSes, page, digitalRead(TOUCH_INT), (unsigned long)irqCount);
  }
  delay(5);
}
