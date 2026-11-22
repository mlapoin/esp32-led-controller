# ESP32 LED Controller

Control a programmable (addressable) LED strip from your phone. The ESP32 joins your Wi-Fi, hosts a small control page, and drives the strip. You don't need an app, a cloud service, or a separate server.

- **Color:** a full color picker plus presets
- **Brightness:** a 0–100% slider
- **Falling snow:** flakes fall down the strip at their own speeds and glide smoothly between LEDs
- **Other effects:** solid, rainbow, breathe, and off
- **Live preview:** the page shows a mirror of the strip, hanging top to bottom
- **Remembers settings:** after a power cut, it comes back the way you left it
- **Safe on USB power:** the strip runs off the ESP32's 5V pin, and the firmware caps the current draw

## Parts

| Part | Notes |
|---|---|
| ESP32 dev board | Any common ESP32 board with the Arduino core |
| WS2812B LED strip | A short strip (the default is 30 LEDs). These are also sold as "NeoPixels" |
| 3 jumper wires | 5V, GND, and data |
| USB cable | Powers both the board and the strip |

> **This needs a programmable (addressable) strip.** Each LED on a WS2812B strip has its own tiny chip, so every LED can be a different color. Regular RGB strips, the kind sold with a remote, change all their LEDs together, usually run on 12V, and need transistors to drive. They won't work with this code. Other addressable types (SK6812, WS2813) work too: change `LED_TYPE` to match.

## Wiring

```
ESP32            LED strip
-----            ---------
5V / VIN  ---->  5V
GND       ---->  GND
GPIO 13   ---->  DIN (data in)
```

The strip takes its power from the ESP32's 5V pin, which comes straight from USB. That works for a short strip, but USB only supplies about 500 mA. Every LED on full white can draw around 60 mA, so 30 LEDs could ask for 1.8 A. To stay safe, the firmware calls `FastLED.setMaxPowerInVoltsAndMilliamps(5, 450)`, which dims the whole strip if a color would draw more than 450 mA. For a longer strip, use a separate 5V supply and connect its ground to the ESP32's ground.

## Setup

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) and add ESP32 support (Boards Manager → "esp32" by Espressif).
2. Install **FastLED** from the Library Manager.
3. Copy `firmware/led_controller/secrets.example.h` to `secrets.h` and fill in your Wi-Fi name and password. `secrets.h` is gitignored.
4. If your strip isn't 30 LEDs on GPIO 13, change `NUM_LEDS` or `DATA_PIN` at the top of `led_controller.ino`.
5. Open `firmware/led_controller/led_controller.ino`, select your board and port, and upload.
6. Open the Serial Monitor at 115200 baud. It prints the address to open, for example `http://192.168.1.42` or `http://leds.local`.

If the ESP32 can't join your Wi-Fi within 15 seconds, it creates its own network called **LED-Controller**. Join that network and open the address shown in the Serial Monitor.

## How it works

```
Phone browser  --Wi-Fi-->  ESP32 (web server + FastLED)  --GPIO 13-->  LED strip
```

- **The page lives on the ESP32.** `web/index.html` is compiled into the firmware as a string (`page.h`), so the board serves it straight from flash.
- **One small API:**
  - `GET /api/state` returns the current settings as JSON.
  - `GET /api/set?color=1e3a8a&brightness=160&effect=snow` changes any of them. Every parameter is optional. Bad values get a `400` with a message instead of being applied.
- **Batched requests:** dragging a slider fires dozens of events a second. The page batches changes and sends them at most every 80 ms, so the ESP32 isn't flooded.
- **Animation loop:** the firmware redraws the strip about 60 times a second. Rainbow shifts the hue each frame, and breathe uses FastLED's `beatsin8` to fade the brightness in and out.
- **Snow:** the strip is treated as hanging with LED 0 at the bottom. Flakes start at the top and fall at their own speeds. Each flake has a position that can sit between two LEDs, and it's drawn across both of them in proportion, so it glides smoothly instead of jumping one LED at a time. The chosen color becomes a dim night-sky tint behind the snow.
- **Saving without wearing out flash:** settings go to the ESP32's flash with `Preferences`. Flash has a limited number of writes, so the firmware waits until two seconds after your last change before saving. Dragging a slider causes one write instead of hundreds.

## Editing the page

Edit `web/index.html`, then rebuild `page.h`:

```bash
python3 tools/embed_page.py
```

You can also open `web/index.html` directly in a browser to work on the layout. It shows "Offline" because there's no ESP32 behind it, but the controls and the preview still work.

## Project layout

```
firmware/led_controller/
  led_controller.ino    firmware: Wi-Fi, web server, LED effects, saving
  page.h                the control page, generated from web/index.html
  secrets.example.h     template for your Wi-Fi details
web/index.html          the control page (edit this one)
tools/embed_page.py     rebuilds page.h from web/index.html
```
