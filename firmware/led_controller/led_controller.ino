// ESP32 LED Controller
// Hosts a small web page on your Wi-Fi so you can change an addressable LED
// strip's color, brightness, and effect from your phone.
//
// Libraries: FastLED (install from the Arduino Library Manager).
// Board: any ESP32 dev board (Arduino core for ESP32).

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <FastLED.h>

#include "secrets.h"  // WIFI_SSID and WIFI_PASSWORD (copy secrets.example.h)
#include "page.h"     // CONTROL_PAGE, built from web/index.html

// ---------- Hardware ----------
#define DATA_PIN    13         // GPIO wired to the strip's DIN
#define NUM_LEDS    30         // how many LEDs are on the strip
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

// The strip is powered from the ESP32's 5V pin, which comes straight from USB.
// USB ports usually give about 500 mA, so FastLED dims the strip automatically
// if a color would pull more than this.
#define MAX_MILLIAMPS 450

#define HOSTNAME   "leds"      // reachable at http://leds.local
#define AP_NAME    "LED-Controller"
#define SAVE_DELAY_MS 2000     // wait this long after the last change before saving
#define FRAME_MS      16       // about 60 frames per second

CRGB leds[NUM_LEDS];
WebServer server(80);
Preferences prefs;

enum Effect : uint8_t { SOLID, SNOW, RAINBOW, BREATHE, OFF, EFFECT_COUNT };
const char* EFFECT_NAMES[] = { "solid", "snow", "rainbow", "breathe", "off" };

struct State {
  CRGB color;
  uint8_t brightness;
  Effect effect;
};

State state = { CRGB(0x1E, 0x3A, 0x8A), 160, SNOW };
bool unsaved = false;
uint32_t lastChangeMs = 0;
uint32_t lastFrameMs = 0;
uint8_t rainbowHue = 0;

// ---------- Helpers ----------

// Turns "ff0066" or "#ff0066" into a color. Returns false if it isn't valid hex.
bool parseHex(String hex, CRGB& out) {
  hex.trim();
  if (hex.startsWith("#")) hex.remove(0, 1);
  if (hex.length() != 6) return false;
  for (size_t i = 0; i < hex.length(); i++) {
    if (!isxdigit((unsigned char)hex[i])) return false;
  }
  long value = strtol(hex.c_str(), nullptr, 16);
  out = CRGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
  return true;
}

String toHex(const CRGB& c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02x%02x%02x", c.r, c.g, c.b);
  return String(buf);
}

bool parseEffect(const String& name, Effect& out) {
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    if (name.equalsIgnoreCase(EFFECT_NAMES[i])) { out = (Effect)i; return true; }
  }
  return false;
}

String stateJson() {
  String json = "{";
  json += "\"color\":\"" + toHex(state.color) + "\",";
  json += "\"brightness\":" + String(state.brightness) + ",";
  json += "\"effect\":\"" + String(EFFECT_NAMES[state.effect]) + "\",";
  json += "\"numLeds\":" + String(NUM_LEDS);
  json += "}";
  return json;
}

// ---------- Saving ----------
// Settings live in flash so the strip comes back the same after a power cut.
// Flash wears out with too many writes, so saves wait until you stop dragging.

void loadState() {
  prefs.begin("leds", true);
  uint32_t rgb = prefs.getUInt("color", 0x1E3A8A);
  state.color = CRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
  state.brightness = prefs.getUChar("bright", 160);
  uint8_t e = prefs.getUChar("effect", SNOW);
  state.effect = e < EFFECT_COUNT ? (Effect)e : SOLID;
  prefs.end();
}

void saveState() {
  prefs.begin("leds", false);
  prefs.putUInt("color", ((uint32_t)state.color.r << 16) | ((uint32_t)state.color.g << 8) | state.color.b);
  prefs.putUChar("bright", state.brightness);
  prefs.putUChar("effect", state.effect);
  prefs.end();
  unsaved = false;
}

// ---------- Web routes ----------

void handleRoot() {
  server.send_P(200, "text/html", CONTROL_PAGE);
}

void handleGetState() {
  server.send(200, "application/json", stateJson());
}

// GET /api/set?color=ff0066&brightness=120&effect=rainbow
// Every parameter is optional, so the page can send just what changed.
void handleSet() {
  if (server.hasArg("color")) {
    CRGB c;
    if (!parseHex(server.arg("color"), c)) {
      server.send(400, "application/json", "{\"error\":\"color must be 6 hex digits\"}");
      return;
    }
    state.color = c;
  }
  if (server.hasArg("brightness")) {
    long b = server.arg("brightness").toInt();
    state.brightness = (uint8_t)constrain(b, 0, 255);
  }
  if (server.hasArg("effect")) {
    Effect e;
    if (!parseEffect(server.arg("effect"), e)) {
      server.send(400, "application/json", "{\"error\":\"effect must be solid, snow, rainbow, breathe, or off\"}");
      return;
    }
    state.effect = e;
  }
  unsaved = true;
  lastChangeMs = millis();
  server.send(200, "application/json", stateJson());
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------- Snow ----------
// The strip hangs with LED 0 at the bottom. Flakes start at the top and fall
// at their own speeds. When one reaches the bottom, it's gone, and new flakes
// keep starting at the top.

#define MAX_FLAKES 8

struct Flake {
  float pos;     // position along the strip, in LEDs (can be between two)
  float speed;   // LEDs per frame
  bool active;
};

Flake flakes[MAX_FLAKES];

const CRGB SNOW_WHITE(210, 220, 255);

void renderSnow() {
  // A dim version of the chosen color is the night sky behind the snow.
  CRGB sky = state.color;
  sky.nscale8_video(40);
  fill_solid(leds, NUM_LEDS, sky);

  // Now and then, start a new flake at the top.
  if (random8() < 8) {
    for (Flake& f : flakes) {
      if (!f.active) {
        f.pos = NUM_LEDS - 1;
        f.speed = 0.05f + random8(50) / 1000.0f;
        f.active = true;
        break;
      }
    }
  }

  for (Flake& f : flakes) {
    if (!f.active) continue;
    f.pos -= f.speed;

    // Fell off the bottom: free this slot for a new flake.
    if (f.pos < 0) {
      f.active = false;
      continue;
    }

    // Split the flake across two LEDs so it glides instead of jumping.
    int i = (int)f.pos;
    uint8_t below = (uint8_t)(255 * (1.0f - (f.pos - i)));
    leds[i] += CRGB(SNOW_WHITE).nscale8_video(below);
    if (i + 1 < NUM_LEDS) leds[i + 1] += CRGB(SNOW_WHITE).nscale8_video(255 - below);
  }
}

// ---------- LEDs ----------

void renderFrame() {
  switch (state.effect) {
    case SOLID:
      fill_solid(leds, NUM_LEDS, state.color);
      break;
    case SNOW:
      renderSnow();
      break;
    case RAINBOW:
      fill_rainbow(leds, NUM_LEDS, rainbowHue++, max(1, 255 / NUM_LEDS));
      break;
    case BREATHE: {
      CRGB c = state.color;
      c.nscale8_video(beatsin8(12, 30, 255));  // slow fade in and out
      fill_solid(leds, NUM_LEDS, c);
      break;
    }
    case OFF:
    default:
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      break;
  }
  FastLED.setBrightness(state.brightness);
  FastLED.show();
}

// ---------- Wi-Fi ----------

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Open http://");
    Serial.print(WiFi.localIP());
    Serial.println(" or http://" HOSTNAME ".local");
  } else {
    // Can't reach the router: make its own network so the page still works.
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_NAME);
    Serial.print("Wi-Fi failed. Join \"" AP_NAME "\" and open http://");
    Serial.println(WiFi.softAPIP());
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  delay(200);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.clear(true);

  loadState();
  startWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/set", HTTP_GET, handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();

  uint32_t now = millis();
  if (now - lastFrameMs >= FRAME_MS) {
    lastFrameMs = now;
    renderFrame();
  }

  if (unsaved && now - lastChangeMs >= SAVE_DELAY_MS) {
    saveState();
  }
}
