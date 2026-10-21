"""Offline test of tools/ota_push.py: urllib.request.urlopen is replaced by a recording fake,
so no socket is opened and no board is needed.

    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_ota_push.py
"""
import contextlib, hashlib, http.client, io, json, os, socket, sys, tempfile, unittest, urllib.error, urllib.request

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import ota_push  # noqa: E402

TOKEN = "dummy-token-0123456789abcdef"
IMAGE = b"\xe9" + b"firmware image bytes " * 40
ELF = b"elf bytes of the build"
ELF8 = hashlib.sha256(ELF).hexdigest()[:8]
OLD8 = "d589e887"
SHA_ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


def put(path, data):
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def ok(what):
    print("ok:", what)


class Resp:
    def __init__(self, status, body):
        self.status, self._b = status, body if isinstance(body, bytes) else body.encode()

    def read(self):
        return self._b

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def info_json(elf=OLD8, state="valid", up=3600.0, version="0.5.2", part="ota_1"):
    return json.dumps({"project": "c6_bulb", "version": version, "elf_sha256": elf,
                       "partition": part, "ota_state": state, "uptime_s": up, "rssi": -50})


class Fake:
    """Stand-in for urlopen. `script` maps a path to a list of replies, used in order:
    (status, body) is returned, or an HTTPError raised when status >= 400."""
    def __init__(self, script):
        self.script, self.calls = script, []

    def __call__(self, req, timeout=None):
        if isinstance(req, str):
            url, method, hdrs, data = req, "GET", {}, None
        else:
            url, method, data = req.full_url, req.get_method(), req.data
            hdrs = {k.lower(): v for k, v in req.header_items()}
        path = "/" + url.split("/", 3)[3]
        self.calls.append({"url": url, "path": path, "method": method, "headers": hdrs, "data": data})
        status, body = self.script[path].pop(0)
        if status >= 400:
            raise urllib.error.HTTPError(url, status, "x", {}, io.BytesIO(body.encode()))
        return Resp(status, body)

    def paths(self):
        return [c["path"] for c in self.calls]


class Base(unittest.TestCase):
    def setUp(self):
        self.old_cwd, self.old_open = os.getcwd(), urllib.request.urlopen
        self.dir = tempfile.TemporaryDirectory()
        os.chdir(self.dir.name)
        os.mkdir("build")
        self.old_env = os.environ.get("ZIGBEE_ANTHEMA_TOKEN")
        os.environ["ZIGBEE_ANTHEMA_TOKEN"] = TOKEN      # the token source of the ordinary tests; the Token tests change it
        ota_push.TOKEN_FILE = None
        put("build/project_description.json", json.dumps({"app_bin": "c6_bulb.bin", "app_elf": "c6_bulb.elf"}))
        put("build/c6_bulb.bin", IMAGE)
        put("build/c6_bulb.elf", ELF)
        self.waited = []
        self.old_wait = ota_push.wait_valid
        ota_push.wait_valid = lambda *a, **k: self.waited.append(a) or 0

    def tearDown(self):
        ota_push.wait_valid = self.old_wait
        ota_push.TOKEN_FILE = None
        if self.old_env is None:
            os.environ.pop("ZIGBEE_ANTHEMA_TOKEN", None)
        else:
            os.environ["ZIGBEE_ANTHEMA_TOKEN"] = self.old_env
        urllib.request.urlopen = self.old_open
        os.chdir(self.old_cwd)
        self.dir.cleanup()

    def run_main(self, script, *argv):
        fake = Fake(script)
        urllib.request.urlopen = fake
        out, code = io.StringIO(), 0
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            try:
                ota_push.main(list(argv))
            except SystemExit as e:
                code = e.code
        return fake, out.getvalue(), code


class Digest(Base):
    def test_fips_vector(self):
        self.assertEqual(ota_push.sha256_of(b"abc"), SHA_ABC)
        ok("sha256_of matches the FIPS 180 vector for 'abc'")


class Token(Base):
    def token_exit(self):
        """token() must exit: its exit code and what it wrote to stderr."""
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
            ota_push.token()
        return cm.exception.code, err.getvalue()

    def test_short_token_exits(self):
        os.environ["ZIGBEE_ANTHEMA_TOKEN"] = "short"
        code, err = self.token_exit()
        self.assertEqual(code, 2)
        self.assertEqual(err.count("\n"), 1)
        self.assertTrue(err.startswith("error: no OTA token"), err)
        self.assertNotIn("short", err.replace("shortcut", ""))
        ok("token() with a short token in the environment: one 'error:' line on stderr, exit 2, the value not echoed")

    def test_missing_token_exits(self):
        os.environ.pop("ZIGBEE_ANTHEMA_TOKEN")
        code, err = self.token_exit()
        self.assertEqual(code, 2)
        self.assertEqual(err.count("\n"), 1)
        self.assertIn("--token", err)
        self.assertIn("ZIGBEE_ANTHEMA_TOKEN", err)
        ok("token() with no file and no environment variable: one 'error:' line naming both sources, exit 2")

    def test_token_from_environment(self):
        self.assertEqual(ota_push.token(), TOKEN)
        os.environ["ZIGBEE_ANTHEMA_TOKEN"] = "  " + TOKEN + "\n"
        self.assertEqual(ota_push.token(), TOKEN)
        ok("token() reads ZIGBEE_ANTHEMA_TOKEN, white space trimmed")

    def test_token_from_file_wins(self):
        other = "file-token-fedcba9876543210"
        put("tok.txt", other + "\n")
        ota_push.TOKEN_FILE = "tok.txt"
        self.assertEqual(ota_push.token(), other)
        ok("token() reads the --token file, and the file wins over the environment variable")

    def test_token_file_problems_exit_without_fallback(self):
        ota_push.TOKEN_FILE = "missing.txt"
        code, err = self.token_exit()
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error: cannot read the --token file"), err)
        self.assertNotIn(TOKEN, err)
        put("short.txt", "tiny\n")
        ota_push.TOKEN_FILE = "short.txt"
        code, err = self.token_exit()
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error: no OTA token"), err)
        self.assertNotIn("tiny", err)
        ok("an unreadable or too-short --token file exits 2 with one 'error:' line and does not fall back to the environment")

    def test_token_with_a_second_line_exits(self):
        put("two.txt", TOKEN + "\n# a second line\n")
        ota_push.TOKEN_FILE = "two.txt"
        code, err = self.token_exit()
        self.assertEqual(code, 2)
        self.assertEqual(err.count("\n"), 1)
        self.assertTrue(err.startswith("error: the token holds a character"), err)
        self.assertNotIn(TOKEN, err)
        ota_push.TOKEN_FILE = None
        os.environ["ZIGBEE_ANTHEMA_TOKEN"] = TOKEN + "\r\nX-Other: 1"
        code, err = self.token_exit()
        self.assertEqual(code, 2)
        self.assertNotIn(TOKEN, err)
        ok("a token holding a line break (a second line in the file, or in the environment) exits 2 with one 'error:' line and is not printed")

    def test_token_file_through_main(self):
        other = "file-token-fedcba9876543210"
        put("tok.txt", other)
        fake, out, code = self.run_main({"/info": [(200, info_json(version="0.6.0"))], "/api/v1/update": [(200, '{"ok":true}')]},
                                        "c6-x.local", "--token", "tok.txt")
        self.assertEqual(code, 0)
        self.assertEqual(fake.calls[1]["headers"]["x-ota-token"], other)
        self.assertEqual(fake.calls[1]["headers"]["authorization"], "Bearer " + other)
        self.assertNotIn(other, out)
        ok("--token <file> on the command line puts the file's token in X-OTA-Token and Authorization: Bearer, and nowhere in the output")

    def test_no_token_sends_nothing(self):
        os.environ.pop("ZIGBEE_ANTHEMA_TOKEN")
        script = {"/info": [(200, info_json(version="0.6.0"))], "/api/v1/update": [(200, "{}")]}
        fake = Fake(script)
        urllib.request.urlopen = fake
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
            ota_push.main(["c6-x.local"])
        self.assertEqual(cm.exception.code, 2)
        self.assertEqual(fake.paths(), ["/info"])
        self.assertEqual(self.waited, [])
        self.assertTrue(err.getvalue().startswith("error: no OTA token"))
        ok("with no token the tool exits 2 before any POST (the read-only /info call is all that went out)")


class Update(Base):
    def script(self, update=((200, '{"ok":true,"version":"0.6.0"}'),), ota=((200, "ok"),), version="0.6.0"):
        return {"/info": [(200, info_json(version=version))], "/api/v1/update": list(update),
                "/ota": list(ota)}

    def test_update_headers(self):
        fake, out, code = self.run_main(self.script(), "c6-x.local")
        self.assertEqual(code, 0)
        self.assertEqual(fake.paths(), ["/info", "/api/v1/update"])
        c = fake.calls[1]
        self.assertEqual(c["method"], "POST")
        self.assertEqual(c["data"], IMAGE)
        self.assertEqual(c["headers"]["x-ota-token"], TOKEN)
        sha = c["headers"]["x-firmware-sha256"]
        self.assertRegex(sha, r"^[0-9a-f]{64}$")
        self.assertEqual(sha, hashlib.sha256(IMAGE).hexdigest())
        self.assertEqual(c["headers"]["authorization"], "Bearer " + TOKEN)
        ok("/api/v1/update gets X-OTA-Token and a 64-hex lower-case X-Firmware-SHA256 of the bytes pushed")
        ok("/api/v1/update carries the same token as Authorization: Bearer, and POSTs the image body")
        self.assertEqual(self.waited[0][1], ELF8)
        ok("the project's own image is proven against the build's ELF digest")

    def test_404_falls_back(self):
        fake, out, code = self.run_main(self.script(update=((404, '{"error":"not_found"}'),)), "c6-x.local")
        self.assertEqual(code, 0)
        self.assertEqual(fake.paths(), ["/info", "/api/v1/update", "/ota"])
        c = fake.calls[2]
        self.assertEqual(c["headers"]["x-ota-token"], TOKEN)
        self.assertEqual(c["headers"]["authorization"], "Bearer " + TOKEN)
        self.assertNotIn("x-firmware-sha256", c["headers"])
        self.assertEqual(c["data"], IMAGE)
        self.assertIn("note: /api/v1/update not found, using /ota", out)
        ok("HTTP 404 from /api/v1/update falls back to /ota with the token in both headers and no digest, and says so")

    def test_old_board_uses_ota_directly(self):
        for ver in ("0.5.2", "0.4.0", "0.5.99"):
            fake, out, code = self.run_main(self.script(version=ver), "c6-x.local")
            self.assertEqual(code, 0)
            self.assertEqual(fake.paths(), ["/info", "/ota"])
            self.assertIn(f"note: board runs {ver}, using /ota", out)
            self.assertNotIn("/api/v1/update", out)
            self.assertNotIn("x-firmware-sha256", fake.calls[1]["headers"])
            self.assertEqual(fake.calls[1]["headers"]["x-ota-token"], TOKEN)
            self.assertEqual(fake.calls[1]["headers"]["authorization"], "Bearer " + TOKEN)
            self.assertEqual(fake.calls[1]["data"], IMAGE)
        ok("a board whose version is below 0.6.0 gets /ota first, with the note line, and no /api/v1/update request")

    def test_new_board_uses_update(self):
        for ver in ("0.6.0", "0.6.1", "1.0.0", "0.10.0"):
            fake, out, code = self.run_main(self.script(version=ver), "c6-x.local")
            self.assertEqual(fake.paths(), ["/info", "/api/v1/update"])
            self.assertNotIn("using /ota", out)
        ok("0.6.0 and later (0.6.1, 0.10.0, 1.0.0) get /api/v1/update")

    def test_unparseable_version_uses_update(self):
        for ver in ("", "unknown", "v0.5.2", None, "0.5"):
            fake, out, code = self.run_main(self.script(version=ver), "c6-x.local")
            self.assertEqual(fake.paths(), ["/info", "/api/v1/update"], ver)
            self.assertNotIn("using /ota", out)
        ok("a version that does not parse gets /api/v1/update")

    def test_old_board_prints_no_sha_line(self):
        fake, out, code = self.run_main(self.script(version="0.5.2"), "c6-x.local")
        self.assertNotIn("sha256:", out)
        ok("an old board's /ota push prints no sha256 line (none is sent)")

    def post_raising(self, exc, argv=("c6-x.local",)):
        """main() with every POST raising exc: (exit code, stdout, stderr, the POSTed paths)."""
        fake = Fake({"/info": [(200, info_json(version="0.6.0"))]})
        real = fake.__call__

        def raising(req, timeout=None):
            if not isinstance(req, str) and req.get_method() == "POST":
                fake.calls.append({"path": "/" + req.full_url.split("/", 3)[3]})
                raise exc
            return real(req, timeout)
        urllib.request.urlopen = raising
        out, err, code = io.StringIO(), io.StringIO(), 0
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = ota_push.main(list(argv))
            except SystemExit as e:
                code = e.code
        return code, out.getvalue(), err.getvalue(), fake.paths()

    def test_url_error_on_post(self):
        for exc in (urllib.error.URLError(ConnectionResetError(104, "Connection reset by peer")),
                    urllib.error.URLError(ConnectionRefusedError(111, "Connection refused")),
                    urllib.error.URLError(socket.timeout("timed out"))):
            code, out, err, paths = self.post_raising(exc)
            msg = str(code)
            self.assertTrue(msg.startswith("error: /api/v1/update: "), msg)
            self.assertNotIn("\n", msg)
            self.assertNotIn("Traceback", out + err)
            self.assertNotIn(TOKEN, out + err + msg)
            self.assertEqual(paths, ["/info", "/api/v1/update"])      # no retry, no fallback
            self.assertEqual(self.waited, [])
        ok("a failure while sending (urllib wraps it in URLError: refused, reset, timeout): one 'error: <path>: <reason>' exit, no traceback, no retry, no wait")

    def test_error_after_the_send_watches_info(self):
        for exc, why in ((ConnectionResetError(104, "Connection reset by peer"), "the connection broke (ConnectionResetError)"),
                         (http.client.RemoteDisconnected("Remote end closed connection without response"), "the connection broke (RemoteDisconnected)"),
                         (OSError("boom"), "the connection broke (OSError)"),
                         (http.client.IncompleteRead(b""), "the answer was cut short (IncompleteRead)"),
                         (http.client.BadStatusLine("HTTP/1."), "the answer was cut short (BadStatusLine)")):
            self.waited.clear()
            code, out, err, paths = self.post_raising(exc)
            self.assertEqual(code, 0)
            self.assertIn(f"note: {why} after the image was sent; watching /info", out)
            self.assertNotIn("push:", out)
            self.assertNotIn("Traceback", out + err)
            self.assertEqual(paths, ["/info", "/api/v1/update"])      # one POST, no retry, no /ota fallback
            self.assertEqual(len(self.waited), 1)
            self.assertEqual(self.waited[0][1], ELF8)
        ok("a bare reset, a closed connection or an answer cut short after the update POST's body went: one note line naming it, no retry, no exit, then wait_valid")

    def test_error_after_the_send_on_ota_still_exits(self):
        for exc in (ConnectionResetError(104, "Connection reset by peer"), http.client.IncompleteRead(b"")):
            code, out, err, paths = self.post_raising(exc, ("c6-x.local", "--legacy"))
            msg = str(code)
            self.assertTrue(msg.startswith("error: /ota: "), msg)
            self.assertNotIn("\n", msg)
            self.assertNotIn("Traceback", out + err)
            self.assertEqual(paths, ["/info", "/ota"])
            self.assertEqual(self.waited, [])
        ok("the same errors on /ota (an older board, or --legacy): one 'error: /ota: ...' exit, no wait")

    def test_update_timeout_watches_info(self):
        calls = []
        fake = Fake({"/info": [(200, info_json(version="0.6.0"))]})
        real = fake.__call__

        def raising(req, timeout=None):
            if not isinstance(req, str) and req.get_method() == "POST":
                calls.append((req.full_url.split("/", 3)[3], timeout))
                raise socket.timeout("timed out")
            return real(req, timeout)
        urllib.request.urlopen = raising
        out, err, code = io.StringIO(), io.StringIO(), 0
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = ota_push.main(["c6-x.local"])
            except SystemExit as e:
                code = e.code
        self.assertEqual(code, 0)
        self.assertEqual(calls, [("api/v1/update", 120)])                    # one POST, no retry, no /ota fallback
        self.assertIn("note: no answer to the push within 120 s; watching /info", out.getvalue())
        self.assertNotIn("push:", out.getvalue())
        self.assertEqual(len(self.waited), 1)
        self.assertEqual(self.waited[0][1], ELF8)
        self.assertNotIn(TOKEN, out.getvalue() + err.getvalue())
        ok("a timeout waiting for the update POST's answer: one note line, no retry, no exit, then wait_valid for this build")

    def test_update_timeout_exit_is_wait_valids(self):
        for outcome in ("ROLLED BACK after 90 s: x", "timed out after 720 s waiting for the new image to report valid"):
            def gone(*a, outcome=outcome, **k):
                raise SystemExit(outcome)
            ota_push.wait_valid = gone
            fake = Fake({"/info": [(200, info_json(version="0.6.0"))]})
            real = fake.__call__

            def raising(req, timeout=None, real=real):
                if not isinstance(req, str) and req.get_method() == "POST":
                    raise socket.timeout("timed out")
                return real(req, timeout)
            urllib.request.urlopen = raising
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as cm:
                ota_push.main(["c6-x.local"])
            self.assertEqual(cm.exception.code, outcome)
        ok("after a timed-out update POST the exit is wait_valid's own (ROLLED BACK / timed out)")

    def test_other_timeouts_still_exit(self):
        # /ota (an old board) and /reboot: the one error line, no wait
        def on_ota(req, timeout=None):
            if isinstance(req, str):
                return Resp(200, info_json(version="0.5.2"))
            raise socket.timeout("timed out")
        urllib.request.urlopen = on_ota
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as cm:
            ota_push.main(["c6-x.local"])
        self.assertEqual(cm.exception.code, "error: /ota: timed out")
        urllib.request.urlopen = lambda req, timeout=None: (_ for _ in ()).throw(socket.timeout("timed out"))
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as cm:
            ota_push.main(["--reboot", "c6-x.local"])
        self.assertEqual(cm.exception.code, "error: /reboot: timed out")
        # /info (before the push): not swallowed, nothing is pushed
        seen = []

        def on_info(req, timeout=None):
            seen.append((req if isinstance(req, str) else req.full_url, timeout))
            raise socket.timeout("timed out")
        urllib.request.urlopen = on_info
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(socket.timeout):
            ota_push.main(["c6-x.local"])
        self.assertEqual(len(seen), 1)
        self.assertTrue(seen[0][0].endswith("/info"))
        self.assertEqual(seen[0][1], 5)
        self.assertEqual(self.waited, [])
        ok("a timeout on /ota, on /reboot or on /info (5 s) still ends the tool; only the update POST's answer is waited on")

    def test_refusal_text_cut_short_is_still_a_refusal(self):
        class Cut(io.BytesIO):
            def read(self, *a):
                raise socket.timeout("timed out")

        def raising(req, timeout=None):
            if isinstance(req, str):
                return Resp(200, info_json(version="0.6.0"))
            raise urllib.error.HTTPError(req.full_url, 413, "x", {}, Cut(b""))
        urllib.request.urlopen = raising
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit) as cm:
            ota_push.main(["c6-x.local"])
        self.assertEqual(cm.exception.code, 1)
        self.assertIn("HTTP 413", out.getvalue())
        self.assertEqual(self.waited, [])
        ok("an HTTP refusal whose text times out while being read is still a refusal: printed, exit 1, no wait")

    def test_url_error_on_ota_names_ota(self):
        def raising(req, timeout=None):
            if isinstance(req, str):
                return Resp(200, info_json(version="0.5.2"))
            raise urllib.error.URLError("no route to host")
        urllib.request.urlopen = raising
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                ota_push.main(["c6-x.local"])
        self.assertEqual(cm.exception.code, "error: /ota: no route to host")
        ok("the error line names the path that failed (/ota here)")

    def test_404_after_reset_is_not_needed_for_old_board(self):
        # a 0.5.x board resets the connection on /api/v1/update; we never send it there
        s = self.script(version="0.5.2", update=())
        fake, out, code = self.run_main(s, "c6-x.local")
        self.assertEqual(code, 0)
        self.assertNotIn("/api/v1/update", fake.paths())
        ok("nothing is sent to /api/v1/update on a 0.5.x board (the reset-on-404 cannot happen)")

    def test_400_prints_body_no_fallback(self):
        body = '{"error":"sha_mismatch","message":"the image\'s SHA-256 does not match X-Firmware-SHA256"}'
        fake, out, code = self.run_main(self.script(update=((400, body),)), "c6-x.local")
        self.assertEqual(code, 1)
        self.assertEqual(fake.paths(), ["/info", "/api/v1/update"])
        self.assertIn("HTTP 400", out)
        self.assertIn(body, out)
        self.assertEqual(self.waited, [])
        ok("a 400 JSON body is printed as returned, exit 1, no /ota fallback, no wait")

    def test_other_refusals_exit_1(self):
        for code_, err in ((409, "wrong_firmware"), (413, "too_large"), (409, "busy")):
            body = json.dumps({"error": err, "message": "m"})
            fake, out, code = self.run_main(self.script(update=((code_, body),)), "c6-x.local")
            self.assertEqual(code, 1)
            self.assertEqual(fake.paths(), ["/info", "/api/v1/update"])
            self.assertIn(f"HTTP {code_}", out)
            self.assertIn(err, out)
        ok("409 wrong_firmware, 413 too_large and 409 busy are printed, exit 1, no fallback")

    def test_legacy(self):
        fake, out, code = self.run_main(self.script(), "c6-x.local", "--legacy")
        self.assertEqual(code, 0)
        self.assertEqual(fake.paths(), ["/info", "/ota"])
        self.assertNotIn("x-firmware-sha256", fake.calls[1]["headers"])
        self.assertNotIn("not found", out)
        ok("--legacy goes straight to /ota (no /api/v1/update request, no note)")

    def test_sha_override(self):
        want = "0" * 64
        fake, out, code = self.run_main(self.script(), "c6-x.local", "--sha", want)
        self.assertEqual(fake.calls[1]["headers"]["x-firmware-sha256"], want)
        self.assertEqual(fake.calls[1]["data"], IMAGE)
        self.assertIn(want, out)
        ok("--sha replaces the digest in the header (the image bytes are unchanged) and is printed")

    def test_sha_validated(self):
        for bad in ("abc", "g" * 64, "0" * 63, "0" * 65):
            fake, out, code = self.run_main(self.script(), "c6-x.local", "--sha", bad)
            self.assertEqual(code, 2)
            self.assertEqual(fake.calls, [])
        ok("--sha that is not exactly 64 hex characters exits 2 before any request")

    def test_file_foreign(self):
        other = os.path.join(self.dir.name, "c6_base.bin")
        put(other, b"another project")
        fake, out, code = self.run_main(self.script(), "c6-x.local", "--file", other)
        self.assertEqual(code, 0)
        self.assertEqual(fake.calls[1]["data"], b"another project")
        self.assertEqual(fake.calls[1]["headers"]["x-firmware-sha256"],
                         hashlib.sha256(b"another project").hexdigest())
        self.assertIn("pushed; not this project's image: no proof", out)
        self.assertIsNone(self.waited[0][1])
        ok("--file reads that path, digests those bytes, and skips the ELF proof with the stated line")

    def test_file_with_project_name_is_proven(self):
        os.mkdir("elsewhere")
        p = os.path.join("elsewhere", "c6_bulb.bin")
        put(p, b"same name")
        fake, out, code = self.run_main(self.script(), "c6-x.local", "--file", p)
        self.assertEqual(fake.calls[1]["data"], b"same name")
        self.assertNotIn("no proof", out)
        self.assertEqual(self.waited[0][1], ELF8)
        ok("--file with the project's app_bin name is still proven")

    def test_file_needs_no_build_tree(self):
        os.remove("build/project_description.json")
        other = os.path.join(self.dir.name, "x.bin")
        put(other, b"zzz")
        fake, out, code = self.run_main(self.script(), "c6-x.local", "--file", other)
        self.assertEqual(code, 0)
        ok("--file works with no build tree (foreign file, no proof)")

    def test_same_build_note(self):
        s = self.script()
        s["/info"] = [(200, info_json(elf=ELF8, version="0.6.0"))]
        fake, out, code = self.run_main(s, "c6-x.local")
        self.assertIn("note: the board already runs this exact build", out)
        ok("a board already on this build gets the note line")

    def test_no_secret_printed(self):
        for argv in (("c6-x.local",), ("c6-x.local", "--legacy")):
            fake, out, code = self.run_main(self.script(), *argv)
            self.assertNotIn(TOKEN, out)
            self.assertNotIn("x-ota-token", out.lower())
            self.assertNotIn("bearer", out.lower())
        fake, out, code = self.run_main({"/reboot": [(200, "rebooting")]}, "--reboot", "c6-x.local")
        self.assertNotIn(TOKEN, out)
        ok("neither the token nor a headers dict appears in any output")

    def test_reboot(self):
        fake, out, code = self.run_main({"/reboot": [(200, "rebooting")]}, "--reboot", "c6-x.local")
        self.assertEqual(fake.paths(), ["/reboot"])
        self.assertEqual(fake.calls[0]["method"], "POST")
        self.assertEqual(fake.calls[0]["headers"]["x-ota-token"], TOKEN)
        self.assertEqual(fake.calls[0]["headers"]["authorization"], "Bearer " + TOKEN)
        self.assertNotIn("x-firmware-sha256", fake.calls[0]["headers"])
        ok("--reboot POSTs /reboot with the token in both headers, no info read")


class Wait(unittest.TestCase):
    """wait_valid on a fake clock: sleep() advances time, /info answers by the clock."""
    def setUp(self):
        self.old = urllib.request.urlopen
        self.t = 1000.0

    def tearDown(self):
        urllib.request.urlopen = self.old

    def run_wait(self, answer, want, before=None, deadline=ota_push.WAIT_S):
        t0 = self.t

        def fake(req, timeout=None):
            r = answer(self.t - t0)
            if r is None:
                raise urllib.error.URLError("unreachable")
            return Resp(200, r)
        urllib.request.urlopen = fake

        def sleep(s):
            self.t += s
        out, code = io.StringIO(), 0
        with contextlib.redirect_stdout(out):
            try:
                ota_push.wait_valid("h", want, before or json.loads(info_json()), t0, deadline,
                                    sleep=sleep, clock=lambda: self.t)
            except SystemExit as e:
                code = e.code
        return out.getvalue(), code

    def test_proof(self):
        def ans(t):
            if t < 40:
                return None
            return info_json(elf=ELF8, up=t - 30, version="0.6.0", part="ota_0")
        out, code = self.run_wait(ans, ELF8)
        self.assertEqual(code, 0)
        self.assertIn("after:", out)
        self.assertIn("0.6.0", out)
        ok("wait_valid: new ELF digest and ota_state valid after a reboot ends with the after: line")

    def test_not_valid_yet_keeps_waiting(self):
        def ans(t):
            return info_json(elf=ELF8, up=t - 5, state="pending_verify", part="ota_0") if t < 100 else \
                info_json(elf=ELF8, up=t - 5, state="valid", part="ota_0")
        out, code = self.run_wait(ans, ELF8)
        self.assertEqual(code, 0)
        self.assertGreaterEqual(self.t - 1000.0, 100)
        ok("wait_valid: pending_verify is not success; it waits for valid")

    def test_old_image_not_taken_for_new(self):
        # before the reboot the old image still answers, with a long uptime: that is not 'after'
        def ans(t):
            if t < 10:
                return info_json(elf=ELF8, up=5000 + t)
            return info_json(elf=ELF8, up=t - 10, part="ota_0")
        out, code = self.run_wait(ans, ELF8, before=json.loads(info_json(elf=ELF8, up=5000)))
        self.assertEqual(code, 0)
        self.assertGreaterEqual(self.t - 1000.0, 10)
        ok("wait_valid: a board still up from before the push is not taken for the new image")

    def test_rollback(self):
        def ans(t):
            if t < 600:
                return info_json(elf="aaaaaaaa", state="pending_verify", up=t - 5, part="ota_0")
            if t < 640:
                return None
            return info_json(elf=OLD8, up=t - 640)
        out, code = self.run_wait(ans, "aaaaaaaa")
        self.assertTrue(str(code).startswith("ROLLED BACK after"), code)
        self.assertLess(self.t - 1000.0, ota_push.WAIT_S)
        ok("wait_valid: the old ELF back after a reboot is reported as ROLLED BACK inside the 720 s deadline")

    def test_same_build_on_other_slot_is_success(self):
        # the same ELF comes back on the other slot: pending_verify first, then valid
        before = json.loads(info_json(elf=ELF8, part="ota_0", up=5000, version="0.6.0"))

        def ans(t):
            if t < 8:
                return info_json(elf=ELF8, part="ota_0", up=5000 + t)
            if t < 15:
                return None
            return info_json(elf=ELF8, part="ota_1", up=t - 15, state="pending_verify" if t < 100 else "valid")
        out, code = self.run_wait(ans, ELF8, before=before)
        self.assertEqual(code, 0)
        self.assertIn("after:", out)
        self.assertNotIn("ROLLED BACK", out)
        self.assertGreaterEqual(self.t - 1000.0, 100)
        ok("wait_valid: the same build back on the other slot is pending, then 'after: ... (valid)', not ROLLED BACK")

    def test_pending_on_new_slot_is_waiting_not_rollback(self):
        before = json.loads(info_json(elf=ELF8, part="ota_0", up=5000))
        out, code = self.run_wait(lambda t: None if t < 15 else
                                  info_json(elf=ELF8, part="ota_1", up=t - 15, state="pending_verify"),
                                  ELF8, before=before, deadline=200)
        self.assertTrue(str(code).startswith("timed out after 200 s"), code)
        ok("wait_valid: pending_verify on the new slot is still waiting (no rollback, no success) up to the deadline")

    def test_rollback_of_same_build_to_old_slot(self):
        # the new slot never became valid; the board returns to the old slot and the old ELF
        before = json.loads(info_json(elf=OLD8, part="ota_0", up=5000))

        def ans(t):
            if t < 8:
                return info_json(elf=OLD8, part="ota_0", up=5000 + t)
            if t < 20:
                return info_json(elf="aaaaaaaa", part="ota_1", up=t - 8, state="pending_verify")
            if t < 30:
                return None
            return info_json(elf=OLD8, part="ota_0", up=t - 30)
        out, code = self.run_wait(ans, "aaaaaaaa", before=before)
        self.assertTrue(str(code).startswith("ROLLED BACK after"), code)
        self.assertIn("on ota_0", str(code))
        ok("wait_valid: back on the old slot with the old ELF after a reboot is ROLLED BACK")

    def test_valid_on_old_slot_after_reboot_is_rollback(self):
        before = json.loads(info_json(elf=ELF8, part="ota_0", up=5000))
        out, code = self.run_wait(lambda t: info_json(elf=ELF8, part="ota_0", up=t - 12) if t > 12 else None,
                                  ELF8, before=before)
        self.assertTrue(str(code).startswith("ROLLED BACK"), code)
        ok("wait_valid: valid on the old slot after a reboot is never success, even with the pushed ELF")

    def test_timeout_and_30s_lines(self):
        out, code = self.run_wait(lambda t: None, ELF8, deadline=95)
        self.assertIn("timed out after 95 s", code)
        lines = [l for l in out.splitlines() if l.strip()]
        self.assertEqual(len(lines), 3, lines)
        self.assertTrue(all("unreachable" in l for l in lines))
        ok("wait_valid: one status line per 30 s ('unreachable' here), then the timeout message")

    def test_status_line_fields(self):
        def ans(t):
            return info_json(elf=OLD8, state="valid", up=5000 + t, version="0.5.2")
        out, code = self.run_wait(ans, ELF8, deadline=65)
        self.assertIn("0.5.2", out)
        self.assertIn("ota_1", out)
        self.assertIn("valid", out)
        ok("wait_valid: the 30 s line shows version, partition, ota_state and uptime")

    def test_foreign_file_ends_on_valid_new_elf(self):
        def ans(t):
            return info_json(elf="bbbbbbbb", up=t - 20, part="ota_0") if t > 25 else None
        out, code = self.run_wait(ans, None)
        self.assertEqual(code, 0)
        self.assertIn("elf not checked", out)
        ok("wait_valid: a foreign file ends on any new valid image, marked 'elf not checked'")

    def test_foreign_file_rollback(self):
        def ans(t):
            if t < 30:
                return None
            return info_json(elf=OLD8, up=t - 30)
        out, code = self.run_wait(ans, None)
        self.assertTrue(str(code).startswith("ROLLED BACK"), code)
        ok("wait_valid: a foreign file that rolls back is still reported as ROLLED BACK")


if __name__ == "__main__":
    unittest.main(verbosity=0)
