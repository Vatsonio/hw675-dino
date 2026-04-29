// dino.ino — Chrome T-Rex game for HW-675 (ESP32-C3 + 0.42" SSD1306 OLED)
// Display: 72x40 visible pixels. Single button on GPIO9 (BOOT, active LOW).
// Library: U8g2 (use the dedicated 72x40 ER constructor — handles offset internally).

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "sprites.h"

// ---------- Hardware pins (HW-675) ----------
static const uint8_t PIN_SDA    = 5;
static const uint8_t PIN_SCL    = 6;
static const uint8_t PIN_BUTTON = 9;
static const uint8_t PIN_LED    = 8;   // active LOW

// ---------- U8g2: dedicated 72x40 ER constructor handles column offset ----------
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ---------- Game constants (local 0..71 x 0..39 coords) ----------
static const int8_t  PLAY_W      = 72;
static const int8_t  PLAY_H      = 40;
static const int8_t  GROUND_Y    = 32;   // y of ground line (entities sit above)
static const int8_t  DINO_X      = 8;
static const int8_t  DINO_W      = 14;
static const int8_t  DINO_H      = 14;
static const int8_t  DINO_GROUND = GROUND_Y - DINO_H;  // y when grounded = 18

// Physics, integer x10 for sub-pixel accuracy — gentle, forgiving feel
static const int16_t GRAVITY     = 5;     // +0.5 px/frame^2  (floaty fall)
static const int16_t JUMP_VEL    = -48;   // -4.8 px/frame    (high but not extreme)
static const int16_t DINO_GROUND10 = (int16_t)DINO_GROUND * 10;
static const int16_t DINO_APEX10   = 0;   // top-of-screen clamp → "hang time" at apex
// → airtime ≈ 18 frames ≈ 600 ms with ~4 frames hang time at the top

// Speed (x10): starts at a brisk pace, ramps gradually, caps at a fair max.
static const int16_t  SPEED_INIT         = 17;   // 1.7 px/frame  (energetic start)
static const int16_t  SPEED_MAX          = 28;   // 2.8 px/frame
static const uint16_t SPEED_STEP_FRAMES  = 180;  // +1 every 6 s
// → reaches max after 11 * 180 = 1980 frames ≈ 66 s

// Obstacle pool — generous gaps so jumps land on a clean rhythm
static const uint8_t  OBS_MAX            = 3;
static const int16_t  MIN_GAP            = 42;
static const int16_t  MAX_GAP            = 80;
// At game start, give the player time to orient before any cactus appears
static const uint16_t GRACE_FRAMES       = 60;   // 2 s of empty road

// Frame timing
static const uint16_t FRAME_MS   = 33;       // ~30 FPS
static const uint16_t ANIM_MS    = 150;      // running-frame swap
static const uint16_t DEBOUNCE_MS = 20;
static const uint16_t RESTART_LOCKOUT_MS = 500;

// ---------- Types ----------
enum GameMode : uint8_t { READY, PLAYING, GAME_OVER };

struct Obstacle {
  int16_t x;        // local px (can go negative)
  uint8_t w;
  uint8_t h;
  bool    active;
  bool    isLarge;  // small vs large cactus
};

struct GameState {
  GameMode  mode;
  int16_t   dinoY10;
  int16_t   vel10;
  Obstacle  obs[OBS_MAX];
  int16_t   speed10;
  int16_t   xAccum;        // sub-pixel accumulator for movement
  uint16_t  score;
  uint16_t  highScore;
  uint8_t   animFrame;     // 0/1 alternating run frame
  uint32_t  animTickMs;
  uint32_t  lastFrameMs;
  uint32_t  gameOverAtMs;
} G;

// ---------- Button (debounced + edge detect) ----------
static bool     btnStable     = false;   // debounced state (true = pressed)
static bool     btnRawPrev    = false;
static uint32_t btnLastEdgeMs = 0;
static bool     btnPressEdge  = false;   // one-shot, consumed by update()

void readButton() {
  bool raw = (digitalRead(PIN_BUTTON) == LOW);  // active LOW
  uint32_t now = millis();
  if (raw != btnRawPrev) {
    btnRawPrev = raw;
    btnLastEdgeMs = now;
  }
  if ((now - btnLastEdgeMs) >= DEBOUNCE_MS && raw != btnStable) {
    btnStable = raw;
    if (btnStable) btnPressEdge = true;
  }
}

// ---------- Game logic ----------
void resetGame() {
  G.mode      = READY;
  G.dinoY10   = DINO_GROUND10;
  G.vel10     = 0;
  G.speed10   = SPEED_INIT;
  G.xAccum    = 0;
  G.score     = 0;
  G.animFrame = 0;
  G.animTickMs = millis();
  for (uint8_t i = 0; i < OBS_MAX; i++) G.obs[i].active = false;
}

void spawnObstacleIfNeeded() {
  // find rightmost active obstacle
  int16_t rightmostX = -1000;
  for (uint8_t i = 0; i < OBS_MAX; i++)
    if (G.obs[i].active && G.obs[i].x > rightmostX) rightmostX = G.obs[i].x;

  int16_t threshold = PLAY_W - (int16_t)random(MIN_GAP, MAX_GAP + 1);
  if (rightmostX < threshold) {
    for (uint8_t i = 0; i < OBS_MAX; i++) {
      if (!G.obs[i].active) {
        bool large = (random(0, 100) < 35);  // 35% large cacti
        G.obs[i].x = PLAY_W;
        G.obs[i].w = large ? 8 : 6;
        G.obs[i].h = large ? 14 : 10;
        G.obs[i].isLarge = large;
        G.obs[i].active = true;
        return;
      }
    }
  }
}

void moveObstacles() {
  G.xAccum += G.speed10;
  int16_t step = G.xAccum / 10;
  G.xAccum -= step * 10;
  for (uint8_t i = 0; i < OBS_MAX; i++) {
    if (!G.obs[i].active) continue;
    G.obs[i].x -= step;
    if (G.obs[i].x + (int16_t)G.obs[i].w < 0) G.obs[i].active = false;
  }
}

bool checkCollision() {
  // dino hitbox: very forgiving (4 px sides, 3 px top/bottom).
  // Visible 14x14 sprite → effective 6x8 hitbox.
  int16_t dx0 = DINO_X + 4;
  int16_t dx1 = DINO_X + DINO_W - 4;
  int16_t dy0 = (G.dinoY10 / 10) + 3;
  int16_t dy1 = (G.dinoY10 / 10) + DINO_H - 3;
  for (uint8_t i = 0; i < OBS_MAX; i++) {
    if (!G.obs[i].active) continue;
    int16_t ox0 = G.obs[i].x;
    int16_t ox1 = G.obs[i].x + G.obs[i].w;
    int16_t oy0 = GROUND_Y - G.obs[i].h;
    int16_t oy1 = GROUND_Y;
    if (dx1 > ox0 && dx0 < ox1 && dy1 > oy0 && dy0 < oy1) return true;
  }
  return false;
}

void update() {
  uint32_t now = millis();

  // Animation frame swap (only matters in PLAYING when grounded)
  if (now - G.animTickMs >= ANIM_MS) {
    G.animFrame ^= 1;
    G.animTickMs = now;
  }

  switch (G.mode) {
    case READY:
      if (btnPressEdge) {
        G.mode  = PLAYING;
        // No forced jump — player gets a clear runway before the first cactus
      }
      break;

    case PLAYING: {
      bool grounded = (G.dinoY10 >= DINO_GROUND10);
      if (btnPressEdge && grounded) {
        G.vel10 = JUMP_VEL;
        grounded = false;
      }
      // gravity
      G.vel10  += GRAVITY;
      G.dinoY10 += G.vel10;
      // ground clamp
      if (G.dinoY10 >= DINO_GROUND10) {
        G.dinoY10 = DINO_GROUND10;
        G.vel10   = 0;
      }
      // apex clamp: keep dino on-screen so the jump peaks visibly with brief hang time
      if (G.dinoY10 < DINO_APEX10) {
        G.dinoY10 = DINO_APEX10;
      }
      // move + spawn + score (no spawning during the initial grace period)
      moveObstacles();
      if (G.score >= GRACE_FRAMES) spawnObstacleIfNeeded();
      G.score++;
      // gradual speed-up to SPEED_MAX
      if ((G.score % SPEED_STEP_FRAMES) == 0 && G.speed10 < SPEED_MAX) G.speed10++;
      // collision
      if (checkCollision()) {
        G.mode = GAME_OVER;
        G.gameOverAtMs = now;
        if (G.score > G.highScore) G.highScore = G.score;
      }
      break;
    }

    case GAME_OVER:
      if ((now - G.gameOverAtMs) >= RESTART_LOCKOUT_MS && btnPressEdge) {
        resetGame();
      }
      break;
  }

  btnPressEdge = false;
}

// ---------- Rendering ----------
void drawGround() {
  // dotted ground line (every 3rd pixel)
  for (int8_t x = 0; x < PLAY_W; x += 3) u8g2.drawPixel(x, GROUND_Y);
}

void drawDino() {
  int16_t y = G.dinoY10 / 10;
  const uint8_t* sprite;
  if (G.mode == GAME_OVER)            sprite = dino_dead;
  else if (G.dinoY10 < DINO_GROUND10) sprite = dino_jump;
  else                                sprite = G.animFrame ? dino_run2 : dino_run1;
  u8g2.drawXBMP(DINO_X, y, DINO_W, DINO_H, sprite);
}

void drawObstacles() {
  for (uint8_t i = 0; i < OBS_MAX; i++) {
    if (!G.obs[i].active) continue;
    int16_t y = GROUND_Y - G.obs[i].h;
    u8g2.drawXBMP(G.obs[i].x, y, G.obs[i].w, G.obs[i].h,
                  G.obs[i].isLarge ? cactus_large : cactus_small);
  }
}

void drawHUD() {
  u8g2.setFont(u8g2_font_4x6_tf);
  // score: divide by 4 for nicer pacing
  uint16_t s = G.score / 4;
  char buf[6];
  snprintf(buf, sizeof(buf), "%05u", s);
  u8g2.drawStr(50, 6, buf);

  if (G.mode == READY) {
    u8g2.drawStr(0, 6, "PRESS");
  } else if (G.mode == GAME_OVER) {
    // Centered "GAME OVER" — 9 chars * 4px + gaps ≈ 36px → x = (72-36)/2 = 18
    u8g2.drawStr(18, 18, "GAME OVER");
    if (G.highScore > 0) {
      char hi[12];
      snprintf(hi, sizeof(hi), "HI:%u", G.highScore / 4);
      u8g2.drawStr(0, 6, hi);
    }
  }
}

void render() {
  u8g2.clearBuffer();
  drawGround();
  drawDino();
  drawObstacles();
  drawHUD();
  u8g2.sendBuffer();
}

// ---------- Arduino entry points ----------
void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);  // off (active LOW)

  Serial.begin(115200);
  delay(50);
  Serial.println(F("HW-675 Dino starting..."));

  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setFontMode(0);
  u8g2.setDrawColor(1);

  // RNG seed from floating analog pin (best we can do without WiFi)
  randomSeed(esp_random());

  resetGame();
  G.lastFrameMs = millis();
}

void loop() {
  readButton();

  uint32_t now = millis();
  if (now - G.lastFrameMs < FRAME_MS) return;
  G.lastFrameMs = now;

  update();
  render();
}
