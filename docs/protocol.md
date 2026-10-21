# Zigbee Anthema — the board's API (v1)

> The Kohler Anthem Plus integration for Home Assistant is a client of this API.

This is the one contract between a Zigbee Anthema board and a program on the LAN. It covers discovery, who
owns a board, the endpoints, the state object and its event stream, the commands, and the errors. Where the
firmware and this file disagree, this file is the intent and the firmware has a bug.

| Section | What it fixes |
|---|---|
| 1 Principles | what holds for every endpoint |
| 2 Discovery | the mDNS record |
| 3 Ownership | unclaimed, claimed, the claim window, release |
| 4 Endpoints | the table and the auth classes |
| 5 State and events | the object and the event stream |
| 6 Commands | light, button, settings, identify, pairing, update |
| 7 Errors | one JSON shape |
| 9 Versions | what each firmware version added |

## 1. Principles

- **Local only.** The board serves the home network: no cloud, no port forwarding.
- **The client connects to the board, never the other way round.** The board never needs the client's address.
  The webhook (§6.3) is an optional bridge for a client that cannot hold the event stream open.
- **One secret per board, given at claim.** After the claim, every write needs it. An unclaimed board accepts a
  claim only in its window (§3).
- **Plain HTTP, token in a header.** The board is an ESP32-C6 with 512 KB of RAM shared by Wi-Fi, the Zigbee
  stack and the HTTP server; a TLS server costs 40–50 KB per connection and flash the image slot has to carry.
  The token travels as `Authorization: Bearer <token>`. Anyone who can read the home network's traffic can read
  it: the API is for a network you trust. The board does not sign requests.
- **One API for every client.** The command-line tools in `tools/` call the same endpoints any other client
  calls.
- **Versioned.** Paths are under `/api/v1`. The board reports `api` in its TXT record and in `/info`; a client
  should refuse an `api` it does not know and say so.
- **Settings live on the board; a mirror's mapping lives in the client.** Role, timings, watchdog and name are the
  board's. Which real lights a board mirrors is the client's concern: the board has no use for them.

## 2. Discovery

mDNS, on port 80:

- **Service type:** `_zigbee-anthema._tcp`.
- **Instance name:** the board's name, `Zigbee Anthema 112233` until renamed (`112233` is the board's `id`).
- **Hostname:** `c6-<id>`, for example `c6-112233`.
- **TXT records:**

| Key | Example | Meaning |
|---|---|---|
| `api` | `1` | API major version |
| `fw` | `0.7.0` | firmware version |
| `id` | `112233` | the serial: the last three bytes of the base MAC, lowercase. Stable; it is also the hostname's suffix and a good unique key for a client |
| `mac` | `AA:BB:CC:11:22:33` | the base MAC, uppercase with colons |
| `hw` | `nanoc6` | the board |
| `state` | `unclaimed` | `unclaimed` or `claimed` |
| `role` | `none` | `none`, `mirror`, `shortcut` — so a discovery card can say what it found |

A client discovers the board by this record. A claimed board is best followed by its `id`, so that a change of
address updates the client rather than creating a second board. The record carries no secret and no
fingerprint.

## 3. Ownership

| State | Who may change the board | `GET /info` says |
|---|---|---|
| **unclaimed, window open** | anyone on the network may claim; nothing else is writable except `identify` | `"claim":"open"` |
| **unclaimed, window closed** | nobody; the user opens the window again | `"claim":"closed"` |
| **claimed** | the token holder | `"claim":"claimed"` |

- **The claim window** is open for **10 minutes after power-up**, and for 10 minutes after **a press of the
  board's button**. A client whose claim is answered `409 window_closed` should tell the user to press the
  button. Physical access is the proof of ownership. The board's light shows no client holds it — a red double
  flash — whether or not the window is open (§6.6).
- **`POST /claim`** (window open, unclaimed): the board makes 32 random bytes, stores their SHA-256 in flash,
  and returns the token once. TXT `state` becomes `claimed`. The client keeps the token.
- **Re-claiming:** a claimed board answers `409 already_claimed`. To take back a board whose token is lost,
  **hold the button for 10 seconds**: the light flashes rapid green from 3 s; at 10 s, with the button still
  down, the board clears the claim only, and the light flashes rapid red until you let go. Wi-Fi, the Zigbee
  pairing and the settings are kept. Then claim again.
- **Release:** `POST /release` (token) undoes the claim. A client calls it, best effort, when the user removes
  the board from it.
- **A `POST /release` opens the claim window for 10 minutes**, so removing the board from a client and adding
  it again needs no walk to the board.
- **A developer credential.** A build may carry a credential in Kconfig (`C6_OTA_TOKEN`), accepted in
  `X-OTA-Token` on `owner` and `token` endpoints beside the claim token. A build with it empty — the public
  default — has only the claim.

## 4. Endpoints

All paths are under `/api/v1`. Request bodies are JSON unless noted. The **Auth** column:

- **none:** anyone.
- **owner:** on an unclaimed board, anyone **without a token** while the window is open; on a claimed board,
  the token. A request that carries a token to an unclaimed board gets `403 claim_required`, window open or
  not: its claim was cleared (the 10 s hold), and the holder must claim again.
- **token:** the token; on an unclaimed board `403 claim_required`.

| Method and path | Auth | Purpose |
|---|---|---|
| `GET /info` | none | identity, versions, claim state, uptime, ip, rssi, partition, `ota_state` |
| `POST /identify` | none | show the board's light for `s` seconds, optionally in a colour `rgb` — which board is this? (§6.6) |
| `GET /log?from=N` | token | the log ring (decoded frames; not for anyone) |
| `POST /claim` | window | lock the board to one client; returns the token |
| `POST /release` | token | undo the claim |
| `GET /state` | owner | the full state, once |
| `GET /events` | owner | the full state, then every change (SSE) |
| `GET /settings` | token | the stored settings, webhook masked |
| `PUT /settings` | token | change settings; the role live (§6.3) |
| `POST /light` | token | the client says what the real light is now (mirror) |
| `POST /button/release` | token | the client says the shortcut ran (shortcut) |
| `POST /zigbee/pair` | token | start steering now (one-tap pairing within the coordinator's window) |
| `POST /zigbee/leave`, `POST /zigbee/erase` | token | leave the network; erase all Zigbee state (re-pair) |
| `POST /update` | token | the firmware image, raw body, `X-Firmware-SHA256` |
| `POST /reboot` | token | reboot |

A few unversioned developer endpoints (`/zb/scan`, `/zb/fc`, `/zb/report`, `/ota`, and `/zb/key` in a build with
`C6_ZB_KEY_ENDPOINT`) exist. Only `/ota` is mentioned again (§6.5); none of them is part of the contract.

## 5. State and events

`GET /state` returns the object once. `GET /events` is Server-Sent Events (`text/event-stream`):

- the first event is `event: state` with the full object;
- then `event: change` with only the top-level keys that changed; each **replaces** that key whole;
- every event has an `id:` — the board's change counter; a gap means a missed event and the client
  reconnects; **the ids on one stream are consecutive** (the `state` event's is the counter, each `change` is
  the last + 1), so merged changes and the 60 s refresh never look like a gap;
- `: ping` every 15 s; a client should treat 45 s of silence as a dead link;
- **every 60 s while a stream is open, a `change` carrying `board` and `zigbee`**, so `uptime_s`, `rssi` and
  `last_rx_s` stay fresh between real changes;
- on reconnect the client gets a fresh full `state`; `Last-Event-ID` is ignored;
- the stream ends on release, reboot or a changed token, with a final `event: end`. **One stream at a time:** a
  second `GET /events` ends the first, because the C6 has few sockets. While the stream is down, a client can
  make one `GET /state` on every failed reconnect attempt, backing off from 1 s doubling to 60 s; a
  successful poll keeps the board available, and the link counts as lost only when the attempt and its poll
  both fail.

**Why SSE and not WebSocket.** On the board it is one chunked response kept open on `esp_http_server`, with no
WebSocket support compiled in. Client to board is plain `POST`s either way.

**The state object:**

```json
{
  "board": {"id": "112233", "name": "Zigbee Anthema 112233", "fw": "0.7.0", "api": 1,
            "hw": "nanoc6", "uptime_s": 8812, "ip": "192.0.2.32", "rssi": -61,
            "ota_state": "valid", "partition": "ota_1"},
  "settings": {"role": "mirror", "webhook": "http://192.0.2.49:8080/hook/ab12…",
               "settle_ms": 2000, "rearm_ms": 3000, "off_after_s": 0, "rx_wd_s": 0, "hue": true},
  "zigbee": {"joined": true, "ieee": "AABBCCFFFE112233", "short": "0x1A2B", "channel": 15,
             "pan": "0x1A62", "ext_pan": "A1B2C3D4E5F60718", "reporting": true,
             "last_rx_s": 4, "profile": "espressif", "hue": true, "group": "0x0001"},
  "light": {"on": 1, "level": 254, "mode": 2, "ct": 250, "x": null, "y": null,
            "hue": null, "sat": null, "by": "zigbee", "at_s": 8800, "unsaved": false},
  "controller": {"seq": 17, "at_s": 8800, "on": 1, "level": 254, "mode": 2, "ct": 250,
                 "settled": true},
  "button": {"held": false, "press": 5, "release_by": "api", "at_s": 8650},
  "update": {"state": "idle", "version": null, "percent": null, "error": null}
}
```

| Key | Values |
|---|---|
| `settings.webhook` | masked: the last path segment cut to 4 characters + `…`; `""` when empty. Never the full id |
| `settings.hue` | `true` or `false`: what a `PUT` last stored. It takes effect at the next reboot or `/zigbee/erase` (§6.3) |
| `light` | the fake bulb's own attributes; `null` = never set. `mode` 0 hue/sat, 1 xy, 2 colour temperature. **`by`** says who set it last: `zigbee` (a command from the Zigbee controller), `api` (`POST /light`), `self` (the button's own Off, `ROLE-OFF`), `restore` (power-up). `at_s` is board uptime at the change |
| `controller` | what the Zigbee controller has asked for so far, the same object the webhook pushes; `seq` counts its commands and is what a mirror reacts to; `settled` is always `true` in the state, which shows the Zigbee controller's target at once |
| `button` | shortcut role: `press` counts presses since boot, `held` while one is pushed and not released, `release_by` `api`/`zigbee`/`timeout`/`none`. A new press is a `change` on `button` with `press` incremented and `held` true. Role `mirror`/`none`: `held` false, counts frozen |
| `zigbee.group` | the Zigbee group the board was added to, from the Zigbee controller's Groups cluster Add Group; `null` before a save. A group move is Remove Group then Add Group, so it follows. Stored in flash, so it survives a reboot |
| `zigbee.reporting` | the On/Off reporting entry is present; `last_rx_s` is seconds since the last frame from the Zigbee controller — the watchdog's view |
| `zigbee.profile` | the identity compiled in: `"espressif"` (the default) or `"sengled_e11"` |
| `zigbee.hue` | the value the Zigbee stack **started with**: whether the colour cluster has hue and saturation, which is what the Zigbee controller can see. The live setting is `settings.hue` |
| `update` | `idle` → `receiving` (`percent`, sent every 5 points and at 100) → `verifying` → `rebooting`; `error` carries the code of a refused image |

**Timestamps are board uptime seconds**, not wall clock: a board may boot before it knows the time.

## 6. Commands

Commands return once the board has acted: `200 {"ok": true}` or an error (§7). The state they led to arrives
on the stream; a client never polls after a command.

### 6.1 `POST /light` — mirror role

The client tells the board what the real light is now, so the Zigbee controller shows it. The board reports to the Zigbee controller; it
never commands it.

```json
{"on": 1, "level": 200, "ct": 300}            or   {"on": 1, "level": 200, "hs": [120, 254]}
```

- `on` 0/1 required; `level` 1–254; one of `ct` (mireds, 1–0xFEFF), `hs` (hue 0–254, sat 0–254) or `xy`
  (0–65279 each); absent fields are left as they are.
- Ignored with `200 {"ok": false, "ignored": "controller_command_3s"}` for 3 s after a Zigbee controller command (the echo
  window) — a client's loop guard can rely on this.
- Also `200 {"ok": false, "ignored": "<detail>"}` when the Zigbee stack refused an attribute write (`hs` while `hue`
  is off, say): `<detail>` names each attribute, the value and its Zigbee status. Only `controller_command_3s` is
  the echo window.
- In role `shortcut`: `409 wrong_role`. In role `none`: applied (nothing listens, but the Zigbee controller sees it).
- Sets `light.by = "api"`.

### 6.2 `POST /button/release` — shortcut role

The client says the shortcut has run; the board reports Off so the Zigbee controller's tile clears.

- No body. `200` if a press was held; `409 no_press` if not; `409 wrong_role` outside the shortcut role.
- The board keeps its own timeout (`off_after_s`) and pushes `release_by: "timeout"` if the client never calls.
  `off_after_s` defaults to **0** (never) in the shortcut role: a press stays held until the client confirms
  (a button) or the Zigbee controller turns it off (a switch). Which of the two a board is in is only a matter of who
  releases, which `release_by` reports.

### 6.3 `GET /settings`, `PUT /settings`

```json
{"role": "shortcut", "settle_ms": 2000, "rearm_ms": 3000, "off_after_s": 0, "rx_wd_s": 0,
 "name": "Guest bath shortcut", "webhook": "http://…", "hue": true}
```

| Key | Type and range | Default |
|---|---|---|
| `role` | `none`, `mirror`, `shortcut` | `none` |
| `settle_ms` | 200–10000 | 2000 |
| `rearm_ms` | 0–30000 | 3000 |
| `off_after_s` | 0 or 10–3600 | 0 (never) |
| `rx_wd_s` | 0 or 30–3600 | 0 (off) |
| `name` | 1–40 characters | `Zigbee Anthema <id>` |
| `webhook` | empty, or an `http://` or `https://` URL of at most 159 characters in all, not ending in `/`; or the literal `"-"` to clear | empty |
| `ext_pan` | 16 hex digits, or empty to join any network; read when the Zigbee stack starts | empty |
| `hue` | a JSON boolean | `true` |

- Every key optional; present keys are validated together, **all or nothing**, and written to flash. `GET`
  also lists `stored`, the keys a `PUT` has written.
- **A role change is live, no reboot.** If the light is on, the board reports Off first (`light.by = "self"`),
  clears the button state, then stores the new role. If the Off cannot be sent: `503 zigbee_busy`, nothing
  stored, old role stands.
- **`hue`** says whether the light's colour cluster offers hue and saturation. With it `true` the Zigbee controller shows
  colour with brightness; with it `false`, brightness only. It accepts `true` or `false` and nothing else (`1`,
  `"on"` and `null` are `400 bad_request`). A `PUT` is stored and answered at once; until the next reboot or
  `POST /zigbee/erase` every `PUT`'s answer carries `"pending":["hue"]`, because the Zigbee stack reads it once, as
  it starts, and the board does not reboot itself. `pending` may list `ext_pan` and `hue`; `GET /settings` and the
  state never carry it (compare `settings.hue` with `zigbee.hue`). **The Zigbee controller reads the
  answer only when it pairs a bulb** — a re-pair of this board, or the pairing of any other — never at once, so
  a change reaches the Zigbee controller's controls at its next pairing.
- Changes arrive on the stream as a `settings` change (webhook masked).

### 6.4 `POST /zigbee/pair`, `/zigbee/leave`, `/zigbee/erase`

- `pair`: start network steering now, within the coordinator's own pairing window (the user has started the
  scan in the coordinator's interface). `200` when steering starts; the join result arrives as a `zigbee`
  change (`joined`, `short`). **On a board that is already joined: `409 already_joined`** — steering on a
  joined device would broadcast a permit-join request and open the coordinator's network for 180 s. A board
  not on a network steers by itself anyway (every 10 s for 15 minutes after boot or after `pair`, then every
  60 s, until it joins); `pair` restarts the fast phase. `pair` answers `503 zigbee_busy` until the stack has
  answered its start this boot, and **`409 rejoining`** while the board is rejoining a stored network (its
  start failed to reach it, or the network sent it a rejoin-type leave), so a board with a stored network is
  never steered. A board whose network never answers again needs `/zigbee/erase` before it can pair with
  another.
- **The board's QR code.** One QR serves every board: the text `ZigbeeAnthema`, in
  `docs/images/pairing-qr.png` in this repository. A coordinator whose web UI wants a QR code to start its scan
  — Kohler Anthem+'s does — takes any content, opens joining to every device, and lists every new device that
  joins. No endpoint is needed for it and a client builds none. `docs/pairing.md` has the rest.
- `leave`: BDB local reset. `erase`: all Zigbee flash state (a changed identity, `hue` or `ext_pan` needs
  this to take effect). Both answer `200` and reboot the stack; the claim and the settings are **kept** (they
  are in `c6cfg`, a different namespace, and the claim in its own).

### 6.5 `POST /update`

- Raw image body, `Content-Length` required; header `X-Firmware-SHA256` (64 hex, either case) required. Token
  class: the claim token, or the developer credential in `X-OTA-Token` (`tools/ota_push.py` sends its one token as both headers).
- Checks before any write: `409 busy` if an update is running (the flag `/ota` shares, so the two never
  interleave); `400 bad_request` for a missing `Content-Length` or a header that is not 64 hex; `413 too_large`
  if the body is larger than the slot.
- The board streams the image into the other slot, hashing as it goes, and then checks in this order: the
  IDF's own image check (`400 bad_image`), the SHA-256 against the header (`400 sha_mismatch`), and the new
  slot's `esp_app_desc_t`: `project_name` must equal **the running image's own name**, not a fixed one, so
  each image accepts only its own kind (`409 wrong_firmware`, whose message names both). A rejected slot is
  simply left; the next update overwrites it. Then it sets the boot partition, answers `200 {"ok": true,
  "version": "0.7.0"}` (the new image's) and reboots after 2 s; the stream's last change carries `rebooting` and
  `version`, then `event: end`. A body that ends early is `400 bad_request`; a flash write or boot-partition
  failure is `500 internal`.
- The first-start check requires Wi-Fi, HTTP **and Zigbee back on the network** within the verification
  window (Zigbee's is ten minutes; a rejoin can take from 20 s to 3 min), else the bootloader rolls
  back.
- Progress: `update` changes on the stream as bytes arrive (`percent`, a change each time it has moved 5
  points, and at 100), then `verifying`, then `rebooting` with `version` set. A refusal after the image
  started puts `update` back to `idle` with `error` set to the code above; it stays until the next update
  starts. After the reboot the stream ends; the client reconnects, reads `board.fw`, and compares. **Before
  0.7.0** `verifying`, `rebooting` and the stream's end were not always delivered, and the 200 could be lost on
  a weak link: a client that updates older boards should read `board.fw` after a timeout.
- **`POST /ota` is kept** (raw body, `X-OTA-Token`, no SHA header, no `update` object): the developer path,
  and the fallback `tools/ota_push.py` uses on a board older than 0.6.0 (`--legacy` forces it).

### 6.6 `POST /identify` — which board is this?

`POST /identify?s=<seconds>&rgb=RRGGBB` shows the board's light for `s` seconds so a person can find it. No
authentication is needed. `rgb` is optional: six hex digits, either case. Without it the light is **white**;
a malformed `rgb` is `400 bad_request` and nothing is shown. The board does not know what a colour means: the
client chooses it (a client may, for example, send the colour of the Zigbee group the board sits in). A board
older than 0.7.0 ignores the parameter and flashes its blue LED, so a client may always send it.

**What the colour LED shows.** When several sources are active, the topmost row of this table wins:

| Source | When | Shows |
|---|---|---|
| identify | `POST /identify` for its seconds | **solid**, in the colour asked for (white when none) |
| hold, 10 s | the button held for 10 s, until let go; the claim is cleared at 10 s | rapid red (8 Hz) |
| hold, 3 s | the button held from 3 s, until let go or 10 s | rapid green (8 Hz) |
| hotspot | the setup hotspot is up (10 minutes after a 3–9 s hold) | green double flash, every 2 s |
| unclaimed | no client holds the board: a fresh board, after a 10 s hold, after `POST /release` — until a client claims it, window open or not | red double flash, every 2 s |
| none | claimed, nothing else going on | off |

The whole LED runs at one brightness (a build option, `C6_RGB_LEVEL`), `rgb` included.

## 7. Errors

Every error the API's handlers send is JSON: `{"error": "<code>", "message": "<human text>"}`. An unknown path or
an unsupported method is answered by the HTTP server itself: `404` or `405` with a plain-text body.

| HTTP | `error` | When |
|---|---|---|
| 400 | `bad_request` | a field is missing or invalid; `message` names the field |
| 401 | `unauthorized` | the token is missing or wrong on a claimed board |
| 403 | `claim_required` | a **token** endpoint on an unclaimed board |
| 409 | `already_claimed` | `/claim` on a claimed board |
| 409 | `window_closed` | `/claim` outside the window; press the button |
| 409 | `wrong_role` | `/light` in the shortcut role, `/button/release` outside it |
| 409 | `no_press` | `/button/release` with nothing held |
| 409 | `already_joined` | `/zigbee/pair` on a board that is on a network |
| 409 | `rejoining` | `/zigbee/pair` while the board rejoins its stored network; erase it to pair with another |
| 409 | `busy` | an update is running |
| 400 | `sha_mismatch` | the image's SHA-256 does not match `X-Firmware-SHA256` |
| 400 | `bad_image` | the image failed the IDF's verification (truncated, not an app image, wrong chip) |
| 409 | `wrong_firmware` | the image is not the kind the board is running (`c6_bulb` on a bulb) |
| 413 | `too_large` | the image is larger than the slot |
| 500 | `storage` | a write to the board's storage failed (settings, claim, or the claim's erase on `/release`); nothing changed |
| 500 | `internal` | the board ran out of memory for the request; try again |
| 503 | `zigbee_busy` | the stack is not up, has not answered its start yet (`/zigbee/pair`), or the lock was busy; try again |

## 9. Versions

What each firmware version added. The API is `api` 1 throughout; a key or endpoint appears at the version that
first has it.

| Version | What it added |
|---|---|
| 0.4.0 | `GET/PUT /api/v1/settings` with a Kconfig token; the role changes live; the IEEE address made from the MAC |
| 0.5.0 | Wi-Fi details kept in flash, the setup box (Improv over USB), the setup hotspot, `board.name`; every-channel scan, join retry, the `zigbee` state keys, `zigbee.group` from the Zigbee controller's Add Group, the `ext_pan` setting; the mDNS record (§2), the claim (§3), `/state`, `/events`, `/light`, `/button/release`, `/zigbee/*`, `/identify`, `/log`, `/reboot` under `/api/v1`; the webhook kept as a setting |
| 0.5.1 | `/zigbee/pair` answers `503 zigbee_busy` until the stack has answered its start; one failed steer no longer ends the search |
| 0.5.2 | `409 rejoining`: a board with a stored network is never steered |
| 0.6.0 | `POST /update` with the SHA and the project check, Zigbee in the first-start check; `off_after_s` defaults to 0; a double blink after a client's release (`blink_ms`), removed again in 0.7.0 |
| 0.7.0 | the rename (`_zigbee-anthema._tcp`, the default name `Zigbee Anthema <id>`, the hotspot `Zigbee-Anthema-<id>`, `"api"` and `"zigbee"` in place of the earlier client and controller values, the state's `controller` object in place of the earlier one, `controller_command_3s` in place of the earlier ignored value, the log tags `API-SET`, `API-RELEASE` and `WEBHOOK`); the update stream ends with `rebooting` and `version`, and the restart waits 2 s; the colour LED's cues and identify's `rgb`; the `hue` setting; the neutral identity and the chip's own IEEE address; one pairing QR; `blink_ms` gone |

**Compatibility note.** Boards before 0.7.0 wrote `"ha"` in three places: `light.by`, `button.release_by` and
the webhook's `by`; from 0.7.0 they write `"api"`. They also wrote `"hub"` where the Zigbee controller made the
change, in the same three places; from 0.7.0 they write `"zigbee"`. And they named the state's object, and the
key of its `change` events, `"hub"`; from 0.7.0 it is `"controller"`. `POST /light`'s echo-window answer
`"ignored": "hub_command_3s"` is `"controller_command_3s"` from 0.7.0. A client that must work with both
accepts both spellings of each. A `PUT /settings` naming a key the board does not know is refused whole, so send
`blink_ms` only to 0.6.0 and `hue` only to 0.7.0 and later.
