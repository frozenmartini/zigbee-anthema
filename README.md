<h1 align="center">Zigbee Anthema</h1>

<p align="center">
  A virtual Zigbee bulb for the ESP32-C6 that bridges to HTTP.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/chip-ESP32--C6-blue" alt="ESP32-C6">
  <img src="https://img.shields.io/badge/licence-MIT-green" alt="Licence: MIT">
</p>

## Zigbee Anthema

Zigbee Anthema is firmware for the ESP32-C6. It joins a Zigbee network as a colour-dimmable light, and
everything the coordinator tells it (on and off, level, colour, colour temperature) is served over a local
REST API and a live event stream. Any program on the LAN can read it, mirror it onto other lights, or treat a
touch on a tile on the Zigbee controller as a button.

It is a bridge, not a lamp: it drives no LED strip and no relay. What a light does is up to the program that
talks to it.

## What it does

The board has three roles, set with the `role` setting and changeable at any time without a reboot or a
re-pair.

- **`none`** — the board joins the network and serves its state. A client may report a light to the Zigbee controller with
  `POST /api/v1/light`; nothing is done with the controller's commands but to show them in the state.
- **`mirror`** — the board stands in for another light. The Zigbee controller's commands arrive in the event stream, and the
  client copies them onto the real light; when the real light changes, the client tells the board with
  `POST /api/v1/light`, and the board reports it to the Zigbee controller, so the controller's tile follows the
  real light.
- **`shortcut`** — the board is a button. A Zigbee controller command that leaves the light on is a press, and the client
  sees it as a press count in the state (or at the webhook). When the client has done its work it calls
  `POST /api/v1/button/release`, and the board reports Off so the tile clears; if the client never does, the
  board's own timeout (`off_after_s`, default never) releases it.

**The webhook** is an optional URL the board posts to (`{"press":N,…}` and `{"release":N,"by":…}`) for a
client that cannot hold the event stream open. The event stream is the main path.

**The claim.** The first client to claim a board owns it: `POST /api/v1/claim` returns a token once, and every
write after that needs it. The claim is open for 10 minutes after power-up and for 10 minutes after a short
press of the board's button. To take a board back whose token is lost, hold the button for 10 seconds: the
claim is cleared, and Wi-Fi, the Zigbee pairing and the settings are kept.

**The button and the light.** The NanoC6's colour LED shows what the button is doing and whether anyone owns
the board.

| You do or see | The light |
|---|---|
| Hold the button, 3 s | rapid green; let go for the setup hotspot |
| The setup hotspot is up (10 minutes) | green double flash |
| Keep holding, 10 s | rapid red: the claim has been cleared; let go |
| No client holds the board (a fresh board, or after a clear) | red double flash, until a client claims it |
| A client asks the board to identify itself | solid, in the colour the client asks for (white if it names none) |
| Claimed, nothing going on | off |

## Hardware

Tested on the M5Stack NanoC6 only. Any ESP32-C6 board with 4 MB of flash should build; the LED and button pins
are Kconfig options (`C6_LED_GPIO`, `C6_BUTTON_GPIO`, `C6_RGB_GPIO`, `C6_RGB_PWR_GPIO`). Wi-Fi and Zigbee share
the chip's one radio; keep the board on constant USB power.

## Install

Open **https://frozenmartini.github.io/zigbee-anthema/** in Chrome or Edge on a computer, plug the board in
with a USB-C cable that carries data (many carry power only), and press Install. The page then asks for your
Wi-Fi details. Not iPhone, iPad or Safari. The steps in words, with what to do when something goes wrong, are
in [`docs/install.md`](docs/install.md); pairing the board to your coordinator is in
[`docs/pairing.md`](docs/pairing.md).

## Tested with

Kohler Anthem+ Zigbee lighting (with the neutral identity, see *Identity*) and the repository's own test
coordinator (`coord/`). **Untested:** Zigbee2MQTT, ZHA, Hue. Reports welcome.

## Identity

The default is neutral. The board presents itself as a colour light with the manufacturer name
`"Zigbee Anthema"`, the model `"zigbee-anthema"`, Espressif's manufacturer code `0x131B`, and the chip's own
IEEE address. Colour is on (the `hue` setting). A profile that copies a Sengled E11-N1EA is a build option
(`C6_ZB_PROFILE_SENGLED_E11`) for a Zigbee controller that admits only that bulb; Kohler Anthem+, the tested system, does not need it. Joining
uses the default link key, with no install code, and one QR code serves every board: see
[`docs/pairing.md`](docs/pairing.md).

## The API

The full contract is [`docs/protocol.md`](docs/protocol.md). The two-minute version, all under `/api/v1`:

- `GET /state` returns everything once.
- `GET /events` returns it again as a Server-Sent Events stream: the full state first, then only what changed.
- `POST /light` tells the board what the real light is now.
- `PUT /settings` changes the role, the timings, the webhook, the name and the `hue` setting.

The board announces itself over mDNS as `_zigbee-anthema._tcp`, and a client claims it with `POST /claim`.

## Updates

`POST /api/v1/update` takes the application image as a raw body with its SHA-256 in `X-Firmware-SHA256`. The
board checks the image, restarts into it, and rolls back to the old one if Wi-Fi, HTTP or Zigbee do not come
back within the verification window (ten minutes for Zigbee). `tools/ota_push.py` does the push from a
command line. A first install from the web page erases the board's pairing; updates do not touch it.

## Adapting it

[`docs/adapting.md`](docs/adapting.md) covers the identity options, the roles, the timings, the webhook, writing
a client, and the tools. To build, with ESP-IDF v5.5:

```sh
cd bulb
idf.py set-target esp32c6
idf.py build
```

The host test harness needs only gcc and python3: `tests/run.sh --all`.

## Used by

The [Kohler Anthem Plus integration for Home Assistant](https://github.com/frozenmartini/kohler-anthem-plus).

## Licence

MIT. See [`LICENSE`](LICENSE).
