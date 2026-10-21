"""Read or change a c6_bulb board over Wi-Fi: settings, state, pairing, events.

Run it from anywhere that can reach the board. The token is the developer token the board was
built with, or the claim token. Give it as a leading --token <file> (a file holding just the
token) or in the environment variable ZIGBEE_ANTHEMA_TOKEN; the file wins. The one token is sent
on every request in both the `X-OTA-Token` header and the `Authorization: Bearer` header, so either
kind works; it is never printed, and neither is any URL. Stdlib only.

    python3 cfg_push.py [--token FILE] <host>                    GET /api/v1/settings
    python3 cfg_push.py [--token FILE] <host> key=value ...      PUT /api/v1/settings, JSON body
    python3 cfg_push.py [--token FILE] <host> --state            GET /api/v1/state
    python3 cfg_push.py [--token FILE] <host> --pair             POST /api/v1/zigbee/pair
    python3 cfg_push.py [--token FILE] <host> --light '<json>'   POST /api/v1/light, e.g. '{"on":1,"level":128}'
    python3 cfg_push.py [--token FILE] <host> --events SECS      GET /api/v1/events, each line printed as
                                                                 it arrives, closed after SECS seconds

Numbers are sent as JSON numbers; role, webhook, name and ext_pan are always strings; hue is a
JSON boolean (hue=true or hue=false, nothing else).
    webhook=-      read one line from stdin into "webhook" (the URL is never an argument)
    webhook=clear  send "webhook":"-", which clears the stored URL

The response body is printed on 200 (a PUT's answer carries a "pending" list naming ext_pan
and/or hue when the stored value differs from the one the stack started with; GET does not). A
settings refusal prints "HTTP <code>: <body>" on stderr; a refusal from --state, --pair,
--light or --events prints "HTTP <code> <error>: <message>" from the board's {"error","message"}
JSON. Either way exit 1: a non-200 is never success.
"""
import http.client, json, os, re, signal, sys, urllib.error, urllib.request

STRING_KEYS = ("role", "webhook", "name", "ext_pan")
BOOL_KEYS = ("hue",)
TOKEN_FILE = None     # set from a leading --token FILE
MODES = ("--state", "--pair", "--light", "--events")


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


def build_body(pairs, stdin):
    body = {}
    for n, pair in enumerate(pairs, 1):
        key, sep, value = pair.partition("=")
        if not sep or not key:
            sys.exit(f"argument {n} after the host is not key=value")   # not echoed: it may be a URL
        if key == "webhook":
            if value == "-":
                value = stdin.readline().rstrip("\r\n")
                if not value:
                    sys.exit("webhook=-: nothing on stdin")
            elif value == "clear":
                value = "-"
            else:
                sys.exit("webhook takes '-' (URL on stdin) or 'clear', never the URL itself")
        elif key in BOOL_KEYS:
            if value not in ("true", "false"):
                sys.exit(f"{key} takes true or false, not {value!r}")
            value = value == "true"
        elif key not in STRING_KEYS and re.fullmatch(r"-?\d+", value):
            value = int(value)
        body[key] = value
    return body


def request(host, body=None, path="/api/v1/settings", method=None, raw=None):
    """One request. body (a dict) is sent as JSON; raw (bytes) is sent as given."""
    tok = token()
    headers = {"X-OTA-Token": tok, "Authorization": f"Bearer {tok}"}
    data = None
    if body is not None:
        data = json.dumps(body, separators=(",", ":")).encode()
    elif raw is not None:
        data = raw
    if data is not None:
        headers["Content-Type"] = "application/json"
    if method is None:
        method = "PUT" if body is not None else "GET"
    req = urllib.request.Request(f"http://{host}{path}", data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode().strip()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode().strip()
    except http.client.HTTPException as e:   # IncompleteRead, BadStatusLine: the answer was cut short
        sys.exit(f"answer from {host} cut short ({type(e).__name__})")
    except OSError as e:
        sys.exit(f"cannot reach {host}: {e.reason if hasattr(e, 'reason') else e}")


def refusal(status, text):
    """'HTTP 409 already_joined: <message>' from the board's error JSON, else 'HTTP <code>: <text>'."""
    try:
        obj = json.loads(text)
        if isinstance(obj, dict) and isinstance(obj.get("error"), str):
            return f"HTTP {status} {obj['error']}: {obj.get('message', '')}".rstrip()
    except ValueError:
        pass
    return f"HTTP {status}: {text}"


def call(host, path, method, raw=None):
    """A new-style command: print the body on 200, the readable refusal on stderr otherwise."""
    status, text = request(host, path=path, method=method, raw=raw)
    if status != 200:
        print(refusal(status, text), file=sys.stderr)
        sys.exit(1)
    print(text)


class Stop(Exception):
    pass


def _stop(signum, frame):
    raise Stop()


def events(host, secs):
    """GET /api/v1/events, each line printed as it arrives, until secs have passed.

    A signal timer ends the read, not a socket timeout: the board sends a ping only every 15 s
    and a timed-out buffered read cannot be resumed.
    """
    tok = token()
    req = urllib.request.Request(f"http://{host}/api/v1/events", method="GET",
                                 headers={"X-OTA-Token": tok, "Authorization": f"Bearer {tok}",
                                          "Accept": "text/event-stream"})
    signal.signal(signal.SIGALRM, _stop)
    signal.setitimer(signal.ITIMER_REAL, secs)
    opened = False
    try:
        with urllib.request.urlopen(req, timeout=secs + 5) as r:
            if r.status != 200:
                print(refusal(r.status, ""), file=sys.stderr)
                sys.exit(1)
            opened = True
            while True:
                line = r.readline()
                if not line:
                    return          # the board ended the stream (event: end)
                sys.stdout.write(line.decode("utf-8", "replace").rstrip("\r\n") + "\n")
                sys.stdout.flush()
    except Stop:
        if not opened:
            sys.exit(f"no stream from {host} within {secs:g} s")
    except urllib.error.HTTPError as e:
        print(refusal(e.code, e.read().decode().strip()), file=sys.stderr)
        sys.exit(1)
    except http.client.HTTPException as e:   # IncompleteRead: the stream was cut in the middle of a chunk
        sys.exit(f"stream from {host} cut short ({type(e).__name__})")
    except OSError as e:
        sys.exit(f"cannot reach {host}: {e.reason if hasattr(e, 'reason') else e}")
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)


def mode(host, argv):
    """argv[0] is one of MODES. Returns after printing; exits non-zero on any refusal."""
    flag, rest = argv[0], argv[1:]
    if flag == "--state" and not rest:
        return call(host, "/api/v1/state", "GET")
    if flag == "--pair" and not rest:
        return call(host, "/api/v1/zigbee/pair", "POST")
    if flag == "--light" and len(rest) == 1:
        try:
            obj = json.loads(rest[0])
        except ValueError:
            sys.exit("--light: not JSON")
        if not isinstance(obj, dict):
            sys.exit("--light: the body must be a JSON object")
        return call(host, "/api/v1/light", "POST", json.dumps(obj, separators=(",", ":")).encode())
    if flag == "--events" and len(rest) == 1:
        try:
            secs = float(rest[0])
        except ValueError:
            sys.exit("--events: SECS must be a number")
        if not 0 < secs <= 3600:
            sys.exit("--events: SECS must be above 0 and at most 3600")
        return events(host, secs)
    sys.exit(f"usage: {flag} " + {"--light": "'<json>'", "--events": "SECS"}.get(flag, "(no arguments)"))


def main(argv):
    global TOKEN_FILE
    if argv[:1] == ["--token"]:
        if len(argv) < 2:
            sys.exit("--token takes a file")
        TOKEN_FILE, argv = argv[1], argv[2:]
    if not argv:
        sys.exit(__doc__)
    host, pairs = argv[0], argv[1:]
    if pairs and pairs[0] in MODES:
        return mode(host, pairs)
    if any(a in MODES for a in pairs):
        sys.exit("--state, --pair, --light and --events stand alone: no key=value with them")
    body = build_body(pairs, sys.stdin) if pairs else None
    status, text = request(host, body)
    if status != 200:
        print(f"HTTP {status}: {text}", file=sys.stderr)
        sys.exit(1)
    print(text)


if __name__ == "__main__":
    main(sys.argv[1:])
