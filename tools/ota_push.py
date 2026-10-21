"""Push the built app to a board over Wi-Fi and wait for it to come back valid.

Run it from a project directory that has a build/ tree (a built bulb/, say), or give the image
with --file. The token is the developer token the board was built with, or the claim token. Give it
as --token <file> (a file holding just the token) or in the environment variable
ZIGBEE_ANTHEMA_TOKEN; the file wins. The one token is sent on every request in both
the `X-OTA-Token` header and the `Authorization: Bearer` header, so either kind works; it is never
shown.

    python3 ota_push.py <host>                    POST /api/v1/update, then watch /info; a board whose
                                                  /info version is below 0.6.0 gets /ota directly
                                                  (note line); on HTTP 404 also falls back to /ota
    python3 ota_push.py <host> --legacy           POST /ota only
    python3 ota_push.py <host> --file <path>      push that file instead of build/<app_bin>
    python3 ota_push.py <host> --sha <64 hex>     send that digest instead of the file's own
    python3 ota_push.py <host> --token <file>     read the token from that file
    python3 ota_push.py --reboot <host>

--file and --sha exist for the refusal tests (wrong project, oversize, wrong digest): the board
is expected to answer 400/409/413 and the tool prints the answer as returned and exits 1.

If the board takes the image but its 200 never arrives (a weak link: the update POST times out, or
the connection breaks after the image was sent), the tool prints one note line and goes on to watch
/info; the outcome is then the board's. A send that failed (refused, reset while sending) still ends
the tool with one error line: the image never went.
"""
import argparse, hashlib, http.client, json, os, re, socket, sys, time, urllib.error, urllib.request

WAIT_S = 720          # the board's 600 s Zigbee verify window + 60 s stable + margin
LINE_EVERY_S = 30
POST_TIMEOUT_S = 120  # seconds the update POST may go without an answer before the tool watches /info
TOKEN_FILE = None     # set from --token


def token():
    """The OTA token: the --token file if one was given, else $ZIGBEE_ANTHEMA_TOKEN. One
    `error:` line and exit 2 when neither gives at least 16 characters. Never printed."""
    tok = ""
    if TOKEN_FILE:
        try:
            with open(os.path.expanduser(TOKEN_FILE)) as f:
                tok = f.read().strip()
        except OSError as e:
            print(f"error: cannot read the --token file: {e.strerror or type(e).__name__}",
                  file=sys.stderr)
            sys.exit(2)
    else:
        tok = os.environ.get("ZIGBEE_ANTHEMA_TOKEN", "").strip()
    if len(tok) < 16:
        print("error: no OTA token (16 characters or more): give --token <file> or set "
              "ZIGBEE_ANTHEMA_TOKEN", file=sys.stderr)
        sys.exit(2)
    if not re.fullmatch(r"[\x20-\x7e]+", tok):   # a header refuses a newline, and its error would show the token
        print("error: the token holds a character a header cannot carry (one line of printable ASCII)",
              file=sys.stderr)
        sys.exit(2)
    return tok


def info(host):
    with urllib.request.urlopen(f"http://{host}/info", timeout=5) as r:
        return json.load(r)


def sha256_of(data):
    return hashlib.sha256(data).hexdigest()


def headers(sha=None):
    """The request headers: the one token as both X-OTA-Token and Authorization: Bearer (the
    developer token is read from the first, the claim token from the second). The result carries
    the token: never output it."""
    tok = token()
    h = {"X-OTA-Token": tok, "Authorization": f"Bearer {tok}",
         "Content-Type": "application/octet-stream"}
    if sha is not None:
        h["X-Firmware-SHA256"] = sha
    return h


def post(host, path, body=b"", sha=None, timeout=POST_TIMEOUT_S):
    """POST and return (status, text). An HTTP error status is returned as an answer; a send or
    connection failure ends the tool with one `error:` line. The one exception is the update POST
    (/api/v1/update): an error after the body was sent (a timeout waiting for the answer, a reset,
    an answer cut short) returns (None, why), because the board may have taken the image and lost
    the 200 on a weak link, so the caller watches /info. urllib wraps every failure while sending in
    URLError, so a bare OSError or HTTPException comes only after the whole body went."""
    req = urllib.request.Request(f"http://{host}{path}", data=body, method="POST",
                                 headers=headers(sha))
    try:
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode().strip()
        except urllib.error.HTTPError as e:
            try:
                try:
                    return e.code, e.read().decode().strip()
                except (OSError, http.client.HTTPException):   # a refusal whose text was cut short is still a refusal
                    return e.code, ""
            finally:
                e.close()
    except urllib.error.URLError as e:   # refused, or failed while sending: the body never went
        sys.exit(f"error: {path}: {e.reason or e}")
    except (OSError, http.client.HTTPException) as e:   # after the body was sent: a timeout (socket.timeout before
        if isinstance(e, (TimeoutError, socket.timeout)):   # Python 3.10 too), a reset, an answer cut short
            why = ""
        elif isinstance(e, OSError):                    # RemoteDisconnected is one too: closed with no answer
            why = f"the connection broke ({type(e).__name__})"
        else:
            why = f"the answer was cut short ({type(e).__name__})"
        if path == "/api/v1/update":
            return None, why
        sys.exit(f"error: {path}: {why or e}")


def pre_update_endpoint(version):
    """True when /info's version parses as lower than 0.6.0 (no /api/v1/update route there).
    A version that does not parse is not 'lower': it gets /api/v1/update."""
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", str(version))
    return bool(m) and tuple(int(x) for x in m.groups()) < (0, 6, 0)


def push(host, image, sha, legacy, version=None):
    """One request per path, chosen before the image is sent: --legacy or a board below 0.6.0
    (`version`, from /info) gets /ota; otherwise /api/v1/update, whose HTTP 404 falls back to
    /ota. Returns (status, text, path); status is None when the update POST's answer was lost (text says
    how, or is empty for a timeout)."""
    if not legacy and pre_update_endpoint(version):
        print(f"note: board runs {version}, using /ota")
        legacy = True
    if not legacy:
        status, text = post(host, "/api/v1/update", image, sha)
        if status != 404:
            return status, text, "/api/v1/update"
        print("note: /api/v1/update not found, using /ota")
    status, text = post(host, "/ota", image)
    return status, text, "/ota"


def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def brief(i):
    return (f"{i['project']} {i['version']} elf {i['elf_sha256']} on {i['partition']} "
            f"({i['ota_state']}), up {i['uptime_s']:.0f} s, rssi {i['rssi']}")


def status_line(i):
    return (f"{i.get('version')} on {i.get('partition')} ({i.get('ota_state')}), "
            f"up {i.get('uptime_s')} s")


def wait_valid(host, want_elf, before, t0, deadline, sleep=time.sleep, clock=time.time):
    """Watch /info for up to `deadline` seconds after t0. want_elf is the first 8 hex of the
    pushed build's ELF digest, or None for a file that is not this project's image (then any
    new image that reports valid ends the wait, unproven). An update always writes the
    other slot: a board that has rebooted and is back on before's partition has rolled back,
    and success needs ota_state valid on a different partition. One status line every 30 s.
    Returns 0 or exits."""
    last_line = t0
    while clock() - t0 < deadline:
        sleep(2)
        try:
            now = info(host)
        except (OSError, ValueError, http.client.HTTPException):   # a reply cut short by the reboot (IncompleteRead) is 'unreachable' too
            now = None                 # unreachable: rebooting, or a new image without Wi-Fi
        if clock() - last_line >= LINE_EVERY_S:
            last_line = clock()
            print(f"  {clock() - t0:5.0f} s: " + (status_line(now) if now else "unreachable"))
        if now is None:
            continue
        rebooted = now["uptime_s"] < clock() - t0
        moved = now.get("partition") != before.get("partition")
        if now["ota_state"] == "valid" and rebooted and moved:
            if want_elf is not None and now["elf_sha256"] == want_elf:
                print(f"after:  {brief(now)}  [{clock() - t0:.1f} s from push]")
                return 0
            if want_elf is None and now["elf_sha256"] != before["elf_sha256"]:
                print(f"after:  {brief(now)}  [{clock() - t0:.1f} s from push; elf not checked]")
                return 0
        if rebooted and not moved:
            sys.exit(f"ROLLED BACK after {clock() - t0:.0f} s: {brief(now)}")
    sys.exit(f"timed out after {deadline} s waiting for the new image to report valid")


def hex64(s):
    if not re.fullmatch(r"[0-9A-Fa-f]{64}", s):
        raise argparse.ArgumentTypeError("--sha takes exactly 64 hex characters")
    return s


def parse(argv):
    p = argparse.ArgumentParser(prog="ota_push.py", description="OTA push to a C6 board")
    p.add_argument("host")
    p.add_argument("--reboot", action="store_true", help="POST /reboot and stop")
    p.add_argument("--legacy", action="store_true", help="push through POST /ota only")
    p.add_argument("--file", metavar="PATH", help="push this file instead of build/<app_bin>")
    p.add_argument("--sha", metavar="HEX64", type=hex64,
                   help="send this digest instead of the file's own (refusal test)")
    p.add_argument("--token", metavar="FILE",
                   help="file holding the OTA token (else $ZIGBEE_ANTHEMA_TOKEN); the developer "
                        "token or the claim token; sent as both X-OTA-Token and "
                        "Authorization: Bearer")
    return p.parse_args(argv)


def main(argv):
    global TOKEN_FILE
    args = parse(argv)
    TOKEN_FILE = args.token
    host = args.host
    if args.reboot:
        print(*post(host, "/reboot"))
        return 0

    try:
        with open("build/project_description.json") as f:
            desc = json.load(f)
    except OSError:
        if not args.file:
            raise
        desc = None                    # a foreign file needs no build tree
    own = None
    if args.file:
        path = os.path.expanduser(args.file)
        if desc and os.path.basename(path) == os.path.basename(desc["app_bin"]):
            own = desc
    else:
        path = "build/" + desc["app_bin"]
        own = desc
    image = slurp(path)
    want = None
    if own:
        want = sha256_of(slurp("build/" + own["app_elf"]))[:8]
    digest = args.sha or sha256_of(image)

    before = info(host)
    print("before:", brief(before))
    if want and before["elf_sha256"] == want:
        print("note: the board already runs this exact build")
    if not (args.legacy or pre_update_endpoint(before.get("version"))):
        print(f"sha256: {digest}")
    t0 = time.time()
    status, text, via = push(host, image, digest, args.legacy, before.get("version"))
    if status is None:
        print((f"note: {text} after the image was sent" if text else
               f"note: no answer to the push within {POST_TIMEOUT_S} s") +
              "; watching /info (if the board never restarts, the image did not land: push again)")
    else:
        print(f"push: {len(image)} bytes to {via}, HTTP {status} in {time.time() - t0:.1f} s: {text}")
        if status != 200:
            sys.exit(1)
    if args.file and not own:
        print("pushed; not this project's image: no proof")
    return wait_valid(host, want, before, t0, WAIT_S)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
