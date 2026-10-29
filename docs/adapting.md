# Adapting Zigbee Anthema

This page is for taking the firmware and making it yours: another identity, another role, a client of your
own. The wire contract is [`protocol.md`](protocol.md).

## Identity (Kconfig)

What the board tells a Zigbee coordinator about itself is chosen at build time, under *c6 bulb (Zigbee end
device)* in `idf.py menuconfig`.

- **The choice `C6_ZB_PROFILE`, "What the bulb presents".**
  - `C6_ZB_PROFILE_ESPRESSIF` is the default: a neutral colour light. Its strings are three options:
    `C6_ZB_MANUFACTURER` (the Basic cluster's ManufacturerName, `"Zigbee Anthema"`), `C6_ZB_MODEL` (the
    ModelIdentifier, `"zigbee-anthema"`) and `C6_ZB_MANUF_CODE` (the node descriptor's manufacturer code,
    `0x131B`, which is Espressif's: a code is registered with the Zigbee Alliance, a name is not). Change the
    two strings freely; keep the code unless you hold a registered one.
  - `C6_ZB_PROFILE_SENGLED_E11` is the other. It is **a build option for a Zigbee controller that admits only a Sengled
    E11-N1EA bulb**: the board then presents that bulb's names, clusters and an address in its range. Kohler Anthem+,
    the tested system, does not need it, and the default build does not use it. If you do not know that you need it, you
    do not.
- **The address.** Under the neutral profile the board's IEEE address is the chip's own. `C6_ZB_IEEE` overrides
  it (16 hex digits, most significant first); leave it empty, and never change the address of a board that is
  already paired.
- **`C6_ZB_INSTALL_CODE`** (36 hex digits: a 128-bit install code and its CRC) makes the board join with an
  install code instead of the default link key. Empty is the default.
- **`C6_ZB_EXT_PANID`** is the default for the `ext_pan` setting: join only this extended PAN ID (16 hex digits).
  Empty, the default, joins any network.
- **`C6_ZB_CHANNEL_MASK`** is the set of channels it may scan and join, bit n for channel n. The default is
  every channel, 11–26.

A paired board keeps the identity it was paired with. After changing any of these, erase its Zigbee state
(`POST /api/v1/zigbee/erase`) and pair it again.

## The `hue` setting

`hue` is a setting (`PUT /api/v1/settings`, a JSON boolean, default `true`) that decides whether the light's
Color Control cluster offers hue and saturation. Right after a new bulb joins, Kohler Anthem+ reads
`CurrentHue` from every bulb it has. A value gives colour together with brightness in Kohler Anthem+'s controls; an
answer of "unsupported attribute" (`0x86`) gives brightness only. With `hue` on (the default) the board answers
with a value; with it off it does not register the attribute, so Kohler Anthem+ gets `0x86`.

Two things to know. The change applies when the Zigbee stack next starts (a reboot or `/zigbee/erase`; the
answer to a `PUT` lists `"pending":["hue"]` until then), and Kohler Anthem+ reads it only when it pairs a bulb, so it
reaches its controls at its next pairing. The role does not change `hue`: a Shortcut ignores colour and
level anyway, so swapping a board's role needs no re-pair.

## The roles

`role` is a setting, changed live with `PUT /api/v1/settings`.

- **`none`** — joined and serving state; nothing else.
- **`mirror`** — stands in for another light. A client follows the Zigbee controller's commands from `GET /events` (the `controller`
  object, whose `seq` counts commands), copies them onto the real light, and tells the board what the real
  light is with `POST /api/v1/light`, which the board reports to the Zigbee controller. A `POST /light` that arrives within
  3 seconds of a Zigbee controller command is ignored (`"ignored":"controller_command_3s"`), so a client that mirrors
  the controller's own command back does not loop.
- **`shortcut`** — a button. The first Zigbee controller command that turns the light on is a press, pushed at once.
  The client sees the press count rise in `button` (and at the webhook), does its work, and calls
  `POST /api/v1/button/release`; the board reports Off so the controller's tile clears. The press is held until
  then, or until the touchscreen turns the light off.

## The receive watchdog

`rx_wd_s` is a setting, with a default in Kconfig used until a value is stored.

| Setting | What it does | Range | Default |
|---|---|---|---|
| `rx_wd_s` | Receive watchdog: reboot after this many seconds without receiving a frame from the Zigbee controller. Armed after the first frame since boot; 0 = off | 0 or 30–3600 | 0 |

## The webhook

The `webhook` setting is an optional URL the board posts JSON to, for a client that cannot hold `GET /events`
open. A press, pushed at the first command that turns the group on:

```json
{"press": 4, "settled": true, "on": 1, "level": 90, "mode": 0, "hue": 0, "sat": 254}
```

A release:

```json
{"release": 4, "by": "zigbee"}
```

`by` is `"zigbee"` (the Zigbee controller turned the light off) or `"api"` (a client called
`POST /api/v1/button/release`).
`GET /settings` shows the URL masked.

## Building a client

A client is a short program. Discover the board by its mDNS record `_zigbee-anthema._tcp` (protocol §2), then:

```sh
# 1. claim it (window open: power-up, or press the button; the token is returned once)
curl -s -X POST http://192.0.2.32/api/v1/claim

# 2. read the state
curl -s -H "Authorization: Bearer $TOKEN" http://192.0.2.32/api/v1/state

# 3. follow it: the full state, then only what changed
curl -sN -H "Authorization: Bearer $TOKEN" http://192.0.2.32/api/v1/events

# 4. tell the Zigbee controller what the real light is (role mirror)
curl -s -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"on":1,"level":200,"ct":300}' http://192.0.2.32/api/v1/light

# 5. change a setting
curl -s -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"role":"shortcut"}' http://192.0.2.32/api/v1/settings
```

`192.0.2.32` stands for your board's address. Keep the token; there is no other way to write to the board
after the claim. If the board answers `403 claim_required` to a request that carries a token, its claim was
cleared by the 10-second hold, and the client must claim again.

## Building the firmware

You need ESP-IDF v5.5 (CI builds the released images with v5.5.5, the version they were tested on).

```sh
cd bulb
idf.py set-target esp32c6
idf.py build
```

The image is `build/c6_bulb.bin`; the project name `c6_bulb` is what an update checks, so each image accepts
only its own kind. `coord/` (a test coordinator) and `sniff/` (an 802.15.4 sniffer) build the same way in their
own directories. `C6_ZB_KEY_ENDPOINT` (off by default) adds `GET /zb/key`, which serves the network key to the
token holder, for reading a sniffer's captures: turn it on only on a test board.

## The host harness

The firmware's logic is tested on a computer, with no board and no ESP-IDF, using gcc and python3:

```sh
tests/run.sh --all
```

`tests/README.md` lists the probes.

## The tools

`tools/ota_push.py` pushes an image to a board. The token is a claim token or a developer credential: both work.
Give it as `--token <file>` (a file holding just the token) or in the environment variable
`ZIGBEE_ANTHEMA_TOKEN`. The tool sends that one token on every request in both the `X-OTA-Token` header and
the `Authorization: Bearer` header (a build reads a developer credential from the first and a claim token from the
second), and never prints it. `tools/cfg_push.py` takes the token and sends it the same way.

Run it from the project directory that holds `build/` (a relative `--token` path is read from there too):

```sh
cd bulb
python3 ../tools/ota_push.py --token token.txt 192.0.2.32
```

- `--file <path>` pushes that file instead of the build output.
- `--sha <hex64>` sends that SHA-256 instead of the file's own (a refusal test).
- `--legacy` pushes through `POST /ota` only, for a board older than 0.6.0.
- `--reboot` sends `POST /reboot` and stops.

It waits for the board to come back and report the new version, and says if the board rolled back.

`tools/cfg_push.py` reads and writes the settings (`key=value` arguments; `hue=true` or `hue=false`), and can
read the state or the event stream. It takes the token the same way.
