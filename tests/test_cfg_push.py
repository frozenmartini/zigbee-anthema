"""Offline test of tools/cfg_push.py: a stdlib HTTP server on 127.0.0.1 stands in for a board.

    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_cfg_push.py
"""
import http.server, json, os, re, subprocess, sys, tempfile, threading, time

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "cfg_push.py")
TOKEN = "dummy-token-0123456789abcdef"
SETTINGS = {"role": "none", "webhook": "", "settle_ms": 2000, "rearm_ms": 3000,
            "off_after_s": 120, "rx_wd_s": 0, "stored": []}
seen = []            # (method, path, token header, content type, body bytes)
accepts = []         # the Accept header of each request, same order as seen
auths = []           # the Authorization header of each request, same order as seen
BEARER = ["Bearer dummy-token-0123456789abcdef"]   # what auths holds after one request that carries TOKEN
STATE = {"on": 1, "level": 128, "joined": 1}
board = {"pair": "ok", "events": "end"}     # what the stand-in does on /zigbee/pair and /events
release = threading.Event()                 # lets a held-open /events handler finish


class Board(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        seen.append((self.command, self.path, self.headers.get("X-OTA-Token"),
                     self.headers.get("Content-Type"), body))
        accepts.append(self.headers.get("Accept"))
        auths.append(self.headers.get("Authorization"))
        if self.path in ROUTES:
            if ROUTES[self.path][0] != self.command:
                return self.reply(405, {"error": "method_not_allowed", "message": "wrong method"})
            if self.headers.get("X-OTA-Token") != TOKEN:
                return self.reply(403, {"error": "unauthorized", "message": "bad token"})
            return getattr(self, ROUTES[self.path][1])(body)
        if self.path != "/api/v1/settings":
            return self.reply(404, {"error": "not_found", "message": "no such endpoint"})
        if self.headers.get("X-OTA-Token") != TOKEN:
            return self.reply(403, {"error": "unauthorized", "message": "bad token"})
        if self.command == "GET":
            return self.reply(200, SETTINGS)
        req = json.loads(body)
        if req.get("settle_ms") == 5:
            return self.reply(400, {"error": "bad_request", "message": "settle_ms out of range"})
        if "ext_pan" in req and not (isinstance(req["ext_pan"], str) and re.fullmatch(r"[0-9a-f]{16}|", req["ext_pan"])):
            return self.reply(400, {"error": "bad_request", "message": "ext_pan: 16 hex digits"})
        if "hue" in req and not isinstance(req["hue"], bool):
            return self.reply(400, {"error": "bad_request", "message": "hue: true or false"})
        out = dict(SETTINGS, **{k: v for k, v in req.items() if k != "webhook"})
        pend = [k for k in ("ext_pan", "hue") if k in req]
        if pend:
            out["pending"] = pend
        return self.reply(200, out)

    def state(self, body):
        self.reply(200, STATE)

    def pair(self, body):
        if board["pair"] == "joined":
            return self.reply(409, {"error": "already_joined",
                                    "message": "leave the network before pairing again"})
        self.reply(200, {"pairing": 1, "seconds": 180})

    def light(self, body):
        req = json.loads(body)
        if req.get("level") == 999:
            return self.reply(400, {"error": "bad_request", "message": "level out of range"})
        if req.get("boom"):         # a refusal that is not the board's error JSON
            data = b"boom"
            self.send_response(500)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            return self.wfile.write(data)
        self.reply(200, dict(STATE, **req))

    def events(self, body):
        if board["events"] == "silent":     # accepts the connection, sends no header for a while
            release.wait(8)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.write(b"event: hello\ndata: {\"on\":1}\n\n")
        self.wfile.flush()
        if board["events"] == "end":        # two more events, then the stream ends
            self.wfile.write(b"event: state\ndata: {\"on\":0}\n\nevent: end\ndata: {}\n\n")
            return
        release.wait(8)                     # "hold": open and silent after the first event

    do_GET = do_PUT = do_POST = handle_any


ROUTES = {"/api/v1/state": ("GET", "state"), "/api/v1/zigbee/pair": ("POST", "pair"),
          "/api/v1/light": ("POST", "light"), "/api/v1/events": ("GET", "events")}


def token_env(src):
    """The environment and the leading arguments that carry a token source to the tool.
    src: None (no token anywhere), ("env", value) in ZIGBEE_ANTHEMA_TOKEN, ("file", value) in a
    --token file, or ("nofile", None) a --token naming a file that does not exist."""
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    env.pop("ZIGBEE_ANTHEMA_TOKEN", None)
    lead = []
    if src and src[0] == "env":
        env["ZIGBEE_ANTHEMA_TOKEN"] = src[1]
    return env, lead


def run(host, args, src, stdin=""):
    seen.clear()
    accepts.clear()
    auths.clear()
    with tempfile.TemporaryDirectory() as d:
        env, lead = token_env(src)
        if src and src[0] == "file":
            with open(os.path.join(d, "token.txt"), "w") as f:
                f.write(src[1] + "\n")
            lead = ["--token", "token.txt"]
        elif src and src[0] == "nofile":
            lead = ["--token", "missing.txt"]
        p = subprocess.run([sys.executable, TOOL] + lead + [host] + args, cwd=d, input=stdin,
                           capture_output=True, text=True, env=env, timeout=30)
    return p.returncode, p.stdout, p.stderr


def check(name, cond):
    if not cond:
        print("FAIL:", name)
        sys.exit(1)
    print("ok:", name)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Board)
threading.Thread(target=srv.serve_forever, daemon=True).start()
host = f"127.0.0.1:{srv.server_address[1]}"
good = ("env", TOKEN)
url = f"http://{host}/api/webhook/test"

# GET
rc, out, err = run(host, [], good)
check("GET exits 0", rc == 0)
check("GET prints the board's JSON", json.loads(out) == SETTINGS)
check("GET is a GET to /api/v1/settings with the token, no body",
      seen == [("GET", "/api/v1/settings", TOKEN, None, b"")])
check("GET sends the same token as Authorization: Bearer too", auths == BEARER)

# PUT, numbers as numbers, role as a string
rc, out, err = run(host, ["role=shortcut", "settle_ms=2500", "off_after_s=0", "rx_wd_s=60"], good)
check("PUT exits 0", rc == 0)
m, path, tok, ctype, body = seen[0]
check("PUT is a PUT to /api/v1/settings with the token and a JSON content type",
      (m, path, tok, ctype) == ("PUT", "/api/v1/settings", TOKEN, "application/json"))
check("PUT sends the same token as Authorization: Bearer too", auths == BEARER)
check("PUT body: numbers as numbers, role as a string",
      json.loads(body) == {"role": "shortcut", "settle_ms": 2500, "off_after_s": 0, "rx_wd_s": 60})
check("PUT prints the response body", json.loads(out)["role"] == "shortcut")

# PUT, webhook from stdin
rc, out, err = run(host, ["webhook=-", "role=mirror"], good, stdin=url + "\n")
check("webhook=- exits 0", rc == 0)
check("webhook=- puts the stdin line in the JSON body, without the newline",
      json.loads(seen[0][4]) == {"webhook": url, "role": "mirror"})
check("the URL is not echoed to stdout or stderr", url not in out and url not in err)
rc, out, err = run(host, ["webhook=-"], good, stdin="")
check("webhook=- with empty stdin fails before any request", rc != 0 and not seen)

# webhook=clear
rc, out, err = run(host, ["webhook=clear"], good)
check("webhook=clear sends \"-\"", rc == 0 and json.loads(seen[0][4]) == {"webhook": "-"})

# a URL as an argument is refused, nothing sent
rc, out, err = run(host, [f"webhook={url}"], good)
check("webhook=<url> on the command line is refused, nothing sent", rc != 0 and not seen)
check("the refusal does not echo the URL", url not in out + err)

# wrong token -> 403 from the board, exit non-zero, body shown, token not shown
rc, out, err = run(host, ["role=none"], ("env", "wrong-token-0123456789abcdef"))
check("wrong token: exit 1", rc == 1)
check("wrong token: HTTP 403 and the board's error JSON on stderr",
      "HTTP 403" in err and '"error": "unauthorized"' in err)
check("wrong token: the token is not printed", "wrong-token" not in out + err)

# no usable token -> no request at all, one `error:` line on stderr, exit 2
rc, out, err = run(host, [], ("env", "short"))
check("a short token in the environment: exit 2, one 'error:' line, nothing sent, the value not printed",
      rc == 2 and not seen and out == "" and err.startswith("error: no OTA token") and err.count("\n") == 1 and "short" not in err.replace("--token", ""))
rc, out, err = run(host, [], None)
check("no token anywhere: exit 2, one 'error:' line naming --token and ZIGBEE_ANTHEMA_TOKEN, nothing sent",
      rc == 2 and not seen and out == "" and err.count("\n") == 1 and "--token" in err and "ZIGBEE_ANTHEMA_TOKEN" in err)

# the token in a --token file
rc, out, err = run(host, [], ("file", TOKEN))
check("--token <file>: the file's token is sent (GET, exit 0)", rc == 0 and seen == [("GET", "/api/v1/settings", TOKEN, None, b"")])
check("--token <file>: the file's token is sent as Authorization: Bearer too", auths == BEARER)
rc, out, err = run(host, ["role=mirror"], ("file", TOKEN))
check("--token <file> with a PUT: the file's token is sent", rc == 0 and seen[0][:3] == ("PUT", "/api/v1/settings", TOKEN))
env, _ = token_env(("env", "wrong-token-0123456789abcdef"))
with tempfile.TemporaryDirectory() as d:
    with open(os.path.join(d, "token.txt"), "w") as f:
        f.write(TOKEN)
    seen.clear()
    p = subprocess.run([sys.executable, TOOL, "--token", "token.txt", host], cwd=d, capture_output=True, text=True, env=env, timeout=30)
check("--token <file> wins over a different ZIGBEE_ANTHEMA_TOKEN", p.returncode == 0 and seen == [("GET", "/api/v1/settings", TOKEN, None, b"")])
rc, out, err = run(host, [], ("nofile", None))
check("--token naming a missing file: exit 2, 'cannot read the --token file', nothing sent",
      rc == 2 and not seen and err.startswith("error: cannot read the --token file") and err.count("\n") == 1)
rc, out, err = run(host, [], ("file", "tiny"))
check("--token file with a short token: exit 2, nothing sent, the value not printed", rc == 2 and not seen and "tiny" not in err)
rc, out, err = run(host, [], ("file", "wrong-token-0123456789abcdef"))
check("--token file with a wrong token: HTTP 403 as for the environment, the token not printed",
      rc == 1 and "HTTP 403" in err and "wrong-token" not in out + err)
seen.clear()
with tempfile.TemporaryDirectory() as d:
    p = subprocess.run([sys.executable, TOOL, "--token"], cwd=d, capture_output=True, text=True, env=token_env(None)[0], timeout=30)
check("--token with no file name: refused, nothing sent", p.returncode != 0 and not seen and "--token takes a file" in p.stderr)

# hue is a JSON boolean; anything else is refused before a request
for word, want in (("true", True), ("false", False)):
    rc, out, err = run(host, ["hue=" + word], good)
    check(f"hue={word} is sent as the JSON boolean {word}",
          rc == 0 and len(seen) == 1 and json.loads(seen[0][4]) == {"hue": want} and json.loads(seen[0][4])["hue"] is want)
    check(f"hue={word}: the answer lists hue as pending", json.loads(out)["pending"] == ["hue"] and json.loads(out)["hue"] is want)
rc, out, err = run(host, ["ext_pan=0123456789abcdef", "hue=false", "settle_ms=2500"], good)
check("hue beside other keys: boolean next to string and number; pending lists ext_pan and hue",
      rc == 0 and json.loads(seen[0][4]) == {"ext_pan": "0123456789abcdef", "hue": False, "settle_ms": 2500} and json.loads(out)["pending"] == ["ext_pan", "hue"])
for bad in ("2", "1", "0", "on", "True", "TRUE", "yes", ""):
    rc, out, err = run(host, ["hue=" + bad], good)
    check(f"hue={bad!r} is refused before any request ('hue takes true or false')",
          rc != 0 and not seen and out == "" and "hue takes true or false" in err)

# a 400 error JSON is echoed so the operator sees the refused key
rc, out, err = run(host, ["settle_ms=5"], good)
check("400: exit 1", rc == 1)
check("400: the board's error JSON is printed, naming the key",
      "HTTP 400" in err and "settle_ms out of range" in err and out == "")

# malformed argument
rc, out, err = run(host, ["settle_ms"], good)
check("an argument without '=' is refused, nothing sent", rc != 0 and not seen)

# ext_pan is a string, even when it is all digits; the answer carries "pending"
rc, out, err = run(host, ["ext_pan=0011223344556677", "name=123", "settle_ms=0011"], good)
check("ext_pan/name stay JSON strings (digits only), settle_ms stays a number",
      rc == 0 and json.loads(seen[0][4]) ==
      {"ext_pan": "0011223344556677", "name": "123", "settle_ms": 11})
check("ext_pan: the body is printed with pending [\"ext_pan\"]",
      json.loads(out)["pending"] == ["ext_pan"] and json.loads(out)["ext_pan"] == "0011223344556677")
rc, out, err = run(host, ["ext_pan=00112233aabbccdd"], good)
check("ext_pan with hex letters is a string too", json.loads(seen[0][4]) == {"ext_pan": "00112233aabbccdd"})
rc, out, err = run(host, ["ext_pan="], good)
check("ext_pan= (empty) is sent as an empty string", rc == 0 and json.loads(seen[0][4]) == {"ext_pan": ""})
rc, out, err = run(host, ["ext_pan=zz"], good)
check("a settings refusal keeps the raw 'HTTP <code>: <body>' line",
      rc == 1 and err.startswith("HTTP 400: {") and "ext_pan: 16 hex digits" in err and out == "")

# --state
rc, out, err = run(host, ["--state"], good)
check("--state exits 0 and prints the board's JSON", rc == 0 and json.loads(out) == STATE)
check("--state is a GET to /api/v1/state with the token, no body",
      seen == [("GET", "/api/v1/state", TOKEN, None, b"")])
check("--state sends the same token as Authorization: Bearer too", auths == BEARER)
rc, out, err = run(host, ["--state"], ("env", "wrong-token-0123456789abcdef"))
check("--state refusal: exit 1, 'HTTP 403 unauthorized: bad token' on stderr, stdout empty",
      rc == 1 and err.strip() == "HTTP 403 unauthorized: bad token" and out == "")
check("--state refusal: the token is not printed", "wrong-token" not in out + err)
rc, out, err = run(host, ["--state", "extra"], good)
check("--state with an argument: usage error, nothing sent", rc != 0 and not seen and "usage" in err)

# --pair
rc, out, err = run(host, ["--pair"], good)
check("--pair exits 0 and prints the answer", rc == 0 and json.loads(out) == {"pairing": 1, "seconds": 180})
check("--pair is a POST to /api/v1/zigbee/pair with the token and no body",
      len(seen) == 1 and seen[0][:3] == ("POST", "/api/v1/zigbee/pair", TOKEN) and seen[0][4] == b"")
check("--pair sends the same token as Authorization: Bearer too", auths == BEARER)
board["pair"] = "joined"
rc, out, err = run(host, ["--pair"], good)
check("--pair 409: exit 1, stderr 'HTTP 409 already_joined: <message>', stdout empty",
      rc == 1 and err.strip() == "HTTP 409 already_joined: leave the network before pairing again"
      and out == "")
board["pair"] = "ok"

# --light
rc, out, err = run(host, ["--light", '{"on": 1, "level": 128}'], good)
check("--light exits 0 and prints the answer", rc == 0 and json.loads(out) == dict(STATE, on=1, level=128))
m, path, tok, ctype, body = seen[0]
check("--light is a POST to /api/v1/light with the token and a JSON content type",
      (m, path, tok, ctype) == ("POST", "/api/v1/light", TOKEN, "application/json"))
check("--light sends the same token as Authorization: Bearer too", auths == BEARER)
check("--light sends the JSON compact, byte for byte", body == b'{"on":1,"level":128}')
rc, out, err = run(host, ["--light", '{"level": 999}'], good)
check("--light 400: exit 1, 'HTTP 400 bad_request: level out of range', stdout empty",
      rc == 1 and err.strip() == "HTTP 400 bad_request: level out of range" and out == "")
rc, out, err = run(host, ["--light", '{"boom": 1}'], good)
check("a refusal that is not the error JSON prints 'HTTP 500: boom'",
      rc == 1 and err.strip() == "HTTP 500: boom" and out == "")
for bad in ("{not json", "[1,2]", "5", '"on"'):
    rc, out, err = run(host, ["--light", bad], good)
    check(f"--light {bad!r}: refused, nothing sent", rc != 0 and not seen and "--light" in err)
rc, out, err = run(host, ["--light"], good)
check("--light without a body: usage error, nothing sent", rc != 0 and not seen and "usage" in err)
rc, out, err = run(host, ["--light", "{}", "{}"], good)
check("--light with two bodies: usage error, nothing sent", rc != 0 and not seen and "usage" in err)

# --events: the stream ends (event: end), is held open past the stop time, or never starts
board["events"] = "end"
rc, out, err = run(host, ["--events", "5"], good)
check("--events exits 0 when the board ends the stream", rc == 0)
check("--events prints every line of the stream, in order, blank lines kept",
      out.splitlines() == ['event: hello', 'data: {"on":1}', '', 'event: state', 'data: {"on":0}', '',
                           'event: end', 'data: {}', ''])
check("--events is a GET to /api/v1/events with the token and Accept: text/event-stream",
      seen == [("GET", "/api/v1/events", TOKEN, None, b"")] and accepts == ["text/event-stream"])
check("--events sends the same token as Authorization: Bearer too", auths == BEARER)

board["events"] = "hold"
release.clear()
t0 = time.time()
rc, out, err = run(host, ["--events", "1"], good)
dt = time.time() - t0
release.set()
check("--events against a silent board stops after SECS: exit 0, first event printed, well before the board lets go",
      rc == 0 and out.splitlines()[:2] == ['event: hello', 'data: {"on":1}'] and dt < 6)
check("--events 1 did not stop at once", dt >= 0.9)

board["events"] = "silent"
release.clear()
t0 = time.time()
rc, out, err = run(host, ["--events", "1"], good)
dt = time.time() - t0
release.set()
check("--events with no stream within SECS: exit 1, 'no stream from', stdout empty",
      rc == 1 and "no stream from" in err and out == "" and dt < 6)

board["events"] = "end"
rc, out, err = run(host, ["--events", "1"], ("env", "wrong-token-0123456789abcdef"))
check("--events refusal: exit 1, 'HTTP 403 unauthorized: bad token', token not printed",
      rc == 1 and err.strip() == "HTTP 403 unauthorized: bad token" and out == ""
      and "wrong-token" not in out + err)

# --events prints each line as it arrives, not at the end
board["events"] = "hold"
release.clear()
with tempfile.TemporaryDirectory() as d:
    p = subprocess.Popen([sys.executable, TOOL, host, "--events", "30"], cwd=d, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=token_env(good)[0])
    guard = threading.Timer(15, p.kill)
    guard.start()
    first = p.stdout.readline().rstrip("\n")
    check("--events: the first line is printed while the stream is still open", first == "event: hello")
    release.set()                       # the board lets go; the tool must end now, not after 30 s
    t0 = time.time()
    rest = p.stdout.read()
    p.wait()
    guard.cancel()
    check("--events ends when the stream ends, without waiting for SECS",
          p.returncode == 0 and time.time() - t0 < 10 and "data: {" in rest)
    p.stdout.close()
    p.stderr.close()
board["events"] = "end"
rc, out, err = run(host, [], ("file", TOKEN + "\n# a second line"))
check("a --token file with a second line: exit 2, one 'error:' line, nothing sent, the token not printed",
      rc == 2 and not seen and out == "" and err.startswith("error: the token holds a character") and err.count("\n") == 1 and TOKEN not in err)

# an argument that is not key=value is refused without echoing it (it may be a URL)
rc, out, err = run(host, ["http://example.invalid/api/webhook/made-up-id"], good)
check("a bare URL as an argument: refused, nothing sent, not echoed",
      rc != 0 and not seen and "example.invalid" not in out + err)

# bad arguments to the new modes: refused, nothing sent
for args, why in ((["--events", "x"], "SECS not a number"), (["--events", "0"], "SECS zero"),
                  (["--events", "-3"], "SECS negative"), (["--events", "3601"], "SECS above 3600"),
                  (["--events"], "SECS missing"), (["--events", "5", "6"], "two SECS")):
    rc, out, err = run(host, args, good)
    check(f"{' '.join(args)}: refused ({why}), nothing sent", rc != 0 and not seen and out == "")
for args in (["--state", "role=none"], ["role=none", "--state"], ["--pair", "settle_ms=1"],
             ["--light", "{}", "role=none"], ["--events", "1", "name=x"], ["role=none", "--events", "1"]):
    rc, out, err = run(host, args, good)
    check(f"mode plus key=value {' '.join(args)}: refused, nothing sent", rc != 0 and not seen and out == "")

# the other modes need the token too: nothing sent without it
for args in (["--state"], ["--pair"], ["--light", "{}"], ["--events", "1"]):
    rc, out, err = run(host, args, ("env", "short"))
    check(f"{args[0]} with a short token: exit 2, nothing sent", rc == 2 and not seen and err.startswith("error: no OTA token"))
    rc, out, err = run(host, args, None)
    check(f"{args[0]} with no token anywhere: exit 2, nothing sent", rc == 2 and not seen and err.startswith("error: no OTA token"))
    rc, out, err = run(host, args, ("file", TOKEN)) if args[0] != "--events" else (0, "", "")
    if args[0] != "--events":
        check(f"{args[0]} with a --token file: the file's token is sent", seen and seen[0][2] == TOKEN)

# board unreachable
srv.shutdown()
srv.server_close()
rc, out, err = run(host, [], good)
check("unreachable board: exit non-zero with a message", rc != 0 and "cannot reach" in err)

print("all passed")
