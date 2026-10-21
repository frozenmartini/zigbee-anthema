# Installing Zigbee Anthema

The install page flashes the board from your browser; there is nothing to download or build. The six steps
below are the page's, in words.

## 1. What you need

- An M5Stack NanoC6 (the board this is tested on).
- A USB-C cable that **carries data**. Many cables carry power only, and a power-only cable is the most common
  reason nothing happens. If in doubt, try another cable.
- A computer with **Chrome or Edge** (Windows, Mac or Linux). The page cannot flash from iPhone, iPad or
  Safari; Firefox is listed by the tool but unverified on this chip.
- Your Wi-Fi name and password. The board uses 2.4 GHz Wi-Fi.

## 2. Plug in, Install, pick the port

Plug the board into the computer, open **https://frozenmartini.github.io/zigbee-anthema/**, press **Install**
and choose the board's serial port in the dialog. The page always writes the whole flash and erases the
pairing and the settings, which is right for a new board. To update a board that is already in use, see the
last section of this page.

## 3. If nothing shows

If the port list is empty, or the page cannot connect:

1. Change the cable (see above).
2. Hold the board's button while you press Install, or while you plug it in, and try again. The page's own
   error message says this too.

## 4. Wi-Fi

Right after flashing, the same page asks for the network name and password and sends them to the board over
the cable (Improv). When the board is on Wi-Fi the page offers **Visit device**, and shows its address.

If you cannot use the box, or you change router later, use the board's own setup hotspot instead:

1. **Hold the button.** After 3 seconds the light flashes rapid green. **Let go while it is rapid green** (do
   not keep holding to 10 seconds: that clears the board's owner instead, see the README).
2. A green double flash means the hotspot is up. It stays up for 10 minutes. It is named
   `Zigbee-Anthema-<id>`.
3. Join it from a phone. A setup page should open by itself. If it doesn't, open `http://192.168.44.1/setup` in the browser. Pick your network and enter the password. The
   board reconnects.

A fresh board, or one whose owner has just been cleared, shows a **red double flash** until a client claims
it. That is normal for a new board and not a fault.

## 5. Pair it to your coordinator

The board is a Zigbee end device. Pair it to your Zigbee controller: see
[`pairing.md`](pairing.md). Pair one board at a time.

## 6. Connect your client

The board announces itself over mDNS as `_zigbee-anthema._tcp` and serves the API described in
[`protocol.md`](protocol.md). A client claims it (`POST /api/v1/claim`) while the claim window is open: the
window is open for 10 minutes after power-up and for 10 minutes after a short press of the button.

## A board that is already in use: update it over Wi-Fi

The install page always writes the whole flash, including the area where the board keeps its Zigbee pairing
and Wi-Fi details, so every install from it erases the pairing and the settings. It has no option to keep
them. Use it for a new board, or when you want to start from nothing.

To move a working board to a newer version, update it over Wi-Fi instead: `POST /api/v1/update`, which never
touches that area. See [Updates in the README](../README.md#updates); `tools/ota_push.py` does the push.

## For the person publishing the page

When you turn on GitHub Pages for this repository, set its source to **GitHub Actions**.
