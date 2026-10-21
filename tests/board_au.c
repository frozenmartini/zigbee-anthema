/* au1-au8: c6_auth.c (the real c6_auth_ok everywhere) and the base endpoints of c6_base.c. A probe builds its claim state with au_state(). */
#define AU_DEV24 "tok-dev-zq9-0123456789ab"       /* 24 chars: a developer credential */
#define AU_DEV15 "tok-zq9-1234567"                /* 15 chars: too short to count */
static void au_digest(const char *tok, uint8_t d[32]) { mbedtls_sha256((const unsigned char *)tok, strlen(tok), d, 0); }
static void au_state(bool claimed, bool open) {
    uint8_t d[32]; au_digest(G_TOKEN, d);
    s_claimed = claimed; if (claimed) memcpy(s_digest, d, 32); else memset(s_digest, 0, 32); g_sha_calls = 0;
    s_window_until_us = open ? fake_now + 600000000LL : 0; g_auto_bearer = 0;
}
static int au_code(httpd_req_t *r, char *code, size_t n) {          /* the status of a JSON answer; its "error" word into code */
    code[0] = 0; if (!r->code) return 0;
    cJSON *j = cJSON_Parse(r->response); const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
    if (e && cJSON_IsString(e)) snprintf(code, n, "%s", e->valuestring);
    cJSON_Delete(j); return r->code;
}
static const char *au_hdr(httpd_req_t *r, const char *k) { return resp_hdr(r, k); }
static const char *g_au_page_trap_note = "";
static sigjmp_buf au_jb;
static void au_segv(int sig) { (void)sig; siglongjmp(au_jb, 1); }
/* How many of the 32 byte positions auth_digest_equal reads: b is laid out so that b[i] is the first byte of an unreadable page, for each i. */
static int au_examined(const uint8_t *a, const uint8_t *b32) {
    long pg = sysconf(_SC_PAGESIZE); uint8_t *base = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { g_au_page_trap_note = "mmap failed"; return -1; }
    mprotect(base + pg, (size_t)pg, PROT_NONE);
    struct sigaction sa, old; memset(&sa, 0, sizeof sa); sa.sa_handler = au_segv; sa.sa_flags = SA_NODEFER; sigaction(SIGSEGV, &sa, &old); sigaction(SIGBUS, &sa, NULL);
    int faults = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t *b = base + pg - i; memcpy(b, b32, (size_t)i);
        if (sigsetjmp(au_jb, 1) == 0) { volatile bool r = auth_digest_equal(a, b); (void)r; } else faults++;
    }
    sigaction(SIGSEGV, &old, NULL); munmap(base, (size_t)pg * 2);
    return faults;
}
static void au_cred(httpd_req_t *r, int cred) { r->h_auth = cred == 1 ? G_BEARER : cred == 2 ? "Bearer wrong-zq9-token-0123456789abcdef" : NULL; }
static const char *au_cname[] = {"NONE", "WINDOW", "OWNER", "TOKEN"};

static void probe_au(void) {
    puts("==== au: claim and auth ====");
    /* au1. the class matrix */
    {   int bad = 0, n = 0;
        for (int cls = 0; cls < 4; cls++) for (int claimed = 0; claimed < 2; claimed++) for (int open = 0; open < 2; open++) for (int cred = 0; cred < 3; cred++) {
            reset_case(); au_state(claimed, open); httpd_req_t r; req_init(&r); au_cred(&r, cred);
            bool ok = c6_auth_ok(&r, (c6_auth_class_t)cls); char code[32]; int st = au_code(&r, code, sizeof code);
            bool want_ok = false; int want_st = 0; const char *want_code = "";
            bool bearer = cred != 0; bool good = claimed && cred == 1;
            switch (cls) {
            case C6_AUTH_NONE: want_ok = true; break;
            case C6_AUTH_WINDOW: if (claimed) { want_st = 409; want_code = "already_claimed"; } else if (open) want_ok = true; else { want_st = 409; want_code = "window_closed"; } break;
            case C6_AUTH_OWNER:
                if (!claimed) { if (bearer || !open) { want_st = 403; want_code = "claim_required"; } else want_ok = true; }
                else if (good) { want_ok = true; } else { want_st = 401; want_code = "unauthorized"; }
                break;
            case C6_AUTH_TOKEN:
                if (!claimed) { want_st = 403; want_code = "claim_required"; } else if (good) { want_ok = true; } else { want_st = 401; want_code = "unauthorized"; }
                break;
            }
            n++;
            int shape = want_ok ? (r.code == 0) : (st == want_st && !strcmp(code, want_code) && !strcmp(r.type, "application/json"));
            if (ok != want_ok || !shape) { bad++; printf("  au1 MISMATCH %s claimed=%d open=%d cred=%d: ok=%d (want %d) status=%d (want %d) code=%s (want %s)\n", au_cname[cls], claimed, open, cred, ok, want_ok, st, want_st, code, want_code); }
        }
        printf("au1 %d combinations (4 classes x 4 states x 3 credentials), mismatches=%d\n", n, bad);
        CHECK(n == 48 && bad == 0);
        reset_case(); au_state(true, false); httpd_req_t r; req_init(&r);
        r.h_auth = "bearer   " G_TOKEN; int lower = c6_auth_ok(&r, C6_AUTH_TOKEN);
        req_init(&r); r.h_auth = "Basic dGVzdDp6cTk="; int basic = c6_auth_ok(&r, C6_AUTH_TOKEN);
        char longh[160]; snprintf(longh, sizeof longh, "Bearer %0100d", 7); req_init(&r); r.h_auth = longh; int longer = c6_auth_ok(&r, C6_AUTH_TOKEN);
        printf("au1 scheme in lower case with extra spaces accepted=%d; Basic refused=%d; an over-long header refused=%d\n", lower, !basic, !longer);
        CHECK(lower && !basic && !longer);
        /* only a Bearer is a bearer attempt; another scheme is no credential, so an unclaimed board whose window is open lets OWNER through */
        reset_case(); au_state(false, true); req_init(&r); r.h_auth = "Basic dGVzdDp6cTk="; int basic_open = c6_auth_ok(&r, C6_AUTH_OWNER) && r.code == 0;
        reset_case(); au_state(false, true); req_init(&r); r.h_auth = "Bearer " G_TOKEN; int bearer_open = c6_auth_ok(&r, C6_AUTH_OWNER); char bc[32]; int bst = au_code(&r, bc, sizeof bc);
        reset_case(); au_state(true, false); req_init(&r); r.h_auth = "Basic dGVzdDp6cTk="; int basic_claimed = c6_auth_ok(&r, C6_AUTH_OWNER); char cc[32]; int cst = au_code(&r, cc, sizeof cc);
        printf("au1 unclaimed, window open, OWNER: a Basic header passes=%d (like no header); a Bearer gets %d %s; claimed with a Basic header: refused=%d %d %s\n", basic_open, bst, bc, !basic_claimed, cst, cc);
        CHECK(basic_open && !bearer_open && bst == 403 && !strcmp(bc, "claim_required") && !basic_claimed && cst == 401 && !strcmp(cc, "unauthorized"));
    }
    verdict("au1", "the class matrix: NONE always; WINDOW only unclaimed and open (else 409 window_closed / already_claimed); OWNER and TOKEN per section 2.5; every refusal a JSON error with its code");

    /* au2. the developer credential */
    {   const char *builds[] = {AU_DEV24, AU_DEV15, ""}; const char *rights[] = {AU_DEV24, AU_DEV15, ""};
        for (int b = 0; b < 3; b++) {
            const char *hdrs[] = {rights[b], "tok-dev-zq9-0123456789ac", "tok-dev-zq9-0123456789a", "tok-dev-zq9-0123456789abc", "", "x"};
            for (int claimed = 0; claimed < 2; claimed++) {
                reset_case(); g_kconfig_ota_token = builds[b]; au_state(claimed, false);
                for (int h = 0; h < 6; h++) {
                    httpd_req_t r; req_init(&r); r.h_ota = hdrs[h]; bool o = c6_auth_ok(&r, C6_AUTH_OWNER);
                    req_init(&r); r.h_ota = hdrs[h]; bool t = c6_auth_ok(&r, C6_AUTH_TOKEN);
                    req_init(&r); r.h_ota = hdrs[h]; bool d = c6_auth_dev_ok(&r);
                    bool want = b == 0 && h == 0;
                    if (o != want || t != want || d != want) { printf("  au2 MISMATCH build %zu chars, claimed=%d, header #%d: owner=%d token=%d dev_ok=%d want %d\n", strlen(builds[b]), claimed, h, o, t, d, want); fails_in_probe++; }
                }
                httpd_req_t r; req_init(&r); r.h_auth = G_BEARER; au_state(true, false); g_auto_bearer = 0;
                int bear = c6_auth_ok(&r, C6_AUTH_OWNER) && c6_auth_ok(&r, C6_AUTH_TOKEN) && c6_auth_dev_ok(&r);
                if (!bear) { printf("  au2 the claim Bearer failed in the %zu-char build\n", strlen(builds[b])); fails_in_probe++; }
            }
        }
        printf("au2 builds of 24, 15 and 0 characters x claimed/unclaimed x six X-OTA-Token values: only the 24-char build with its exact value accepted\n");
    }
    verdict("au2", "the developer credential: a 24-char CONFIG_C6_OTA_TOKEN accepts only its exact X-OTA-Token on OWNER, TOKEN and c6_auth_dev_ok; 15 chars or empty refuse every value; the claim Bearer works in all three");

    /* au3. POST /claim */
    {   reset_case(); for (int i = 0; i < 32; i++) g_rand_seq[i] = (uint8_t)(i * 9 + 3); g_rand_seq_n = 32;
        g_auto_bearer = 0; s_claimed = false; memset(s_digest, 0, 32); c6_discovery_start(); c6_auth_start();
        int led_before = (s_active >> C6_LED_UNCLAIMED) & 1; char want[65]; for (int i = 0; i < 32; i++) snprintf(want + 2 * i, 3, "%02x", (uint8_t)(i * 9 + 3));
        httpd_req_t r; req_init(&r); api_call(api_claim_post, &r);
        unsigned char in[300]; size_t inlen = g_sha_len; memcpy(in, g_sha_in, inlen < 300 ? inlen : 300);
        cJSON *j = cJSON_Parse(r.response); const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "token") : NULL;
        int hexok = t && cJSON_IsString(t) && strlen(t->valuestring) == 64; if (hexok) for (int i = 0; i < 64; i++) hexok &= (t->valuestring[i] >= '0' && t->valuestring[i] <= '9') || (t->valuestring[i] >= 'a' && t->valuestring[i] <= 'f');
        int exact = hexok && !strcmp(t->valuestring, want); cJSON_Delete(j);
        uint8_t d[32]; au_digest(want, d); int bi = fs_find("c6claim", "sha");
        int blob32 = bi >= 0 && g_fs[bi].type == 4 && g_fs[bi].n == 32 && !memcmp(g_fs[bi].b, d, 32);
        uint8_t keep[32]; if (bi >= 0) memcpy(keep, g_fs[bi].b, 32);
        printf("au3 claim: http=%d token 64 lowercase hex=%d equals the scripted bytes=%d; digest input %zu bytes equal to the token=%d; blob 32 bytes = digest=%d; token in the blob=%d; TXT state=%s; UNCLAIMED LED before/after=%d/%d; random calls=%d\n",
               r.code, hexok, exact, inlen, inlen == 64 && !memcmp(in, want, 64), blob32, bi >= 0 && memmem(g_fs[bi].b, g_fs[bi].n, want, 8) != NULL, mdns_txt("state"), led_before, (s_active >> C6_LED_UNCLAIMED) & 1, g_rand_calls);
        CHECK(r.code == 200 && exact && inlen == 64 && !memcmp(in, want, 64) && blob32 && !(bi >= 0 && memmem(g_fs[bi].b, g_fs[bi].n, want, 8)) && mdns_txt("state") && !strcmp(mdns_txt("state"), "claimed")
              && led_before == 1 && !((s_active >> C6_LED_UNCLAIMED) & 1) && g_rand_calls == 1 && c6_auth_claimed());
        int rc = g_rand_calls; httpd_req_t r2; req_init(&r2); api_call(api_claim_post, &r2); char code[32]; int st = au_code(&r2, code, sizeof code);
        printf("au3 second claim: %d %s; random calls %d -> %d; blob unchanged=%d\n", st, code, rc, g_rand_calls, bi >= 0 && !memcmp(g_fs[bi].b, keep, 32));
        CHECK(st == 409 && !strcmp(code, "already_claimed") && g_rand_calls == rc && !memcmp(g_fs[bi].b, keep, 32));
        reset_case(); g_auto_bearer = 0; s_claimed = false; c6_auth_start(); fake_advance(601000000); httpd_req_t r3; req_init(&r3); api_call(api_claim_post, &r3);
        st = au_code(&r3, code, sizeof code);
        printf("au3 closed and unclaimed: %d %s; random calls=%d; stored=%d\n", st, code, g_rand_calls, fs_find("c6claim", "sha") >= 0);
        CHECK(st == 409 && !strcmp(code, "window_closed") && g_rand_calls == 0 && fs_find("c6claim", "sha") < 0);
    }
    verdict("au3", "POST /claim in the window: 200 with 64 lowercase hex; the digest of exactly those 64 bytes stored as a 32-byte blob, never the token; TXT claimed; UNCLAIMED LED off; a second claim and a closed window are 409 with no new random");

    /* au4. the digest compare reads all 32 bytes, whatever differs */
    {   uint8_t D[32], x[32]; for (int i = 0; i < 32; i++) D[i] = (uint8_t)(i * 5 + 1);
        const char *nm[] = {"byte 0 only", "byte 31 only", "every byte", "none"}; int ex[4]; int pass[4];
        reset_case(); au_state(true, false); memcpy(s_digest, D, 32); g_sha_use_fixed = true;
        for (int k = 0; k < 4; k++) {
            memcpy(x, D, 32); if (k == 0) x[0] ^= 0x80; if (k == 1) x[31] ^= 0x01; if (k == 2) for (int i = 0; i < 32; i++) x[i] = (uint8_t)~D[i];
            ex[k] = au_examined(x, D);                                            /* a = the presented digest, b = the stored one; b[i] lies at an unreadable page, for each i */
            memcpy(g_sha_fixed, x, 32); httpd_req_t r; req_init(&r); r.h_auth = G_BEARER; pass[k] = c6_auth_ok(&r, C6_AUTH_OWNER);
            char code[32]; int st = au_code(&r, code, sizeof code);
            printf("au4 digest differing in %s: examined bytes of the compare=%d; accepted=%d (%d %s)\n", nm[k], ex[k], pass[k], st, code);
            CHECK(ex[k] == 32 && pass[k] == (k == 3) && (k == 3 || (st == 401 && !strcmp(code, "unauthorized"))));
        }
        reset_case(); uint8_t b31[31]; memset(b31, 7, 31); fs_put("c6claim", "sha", 4, 0, b31, 31); size_t m = g_loglen; au_boot();
        int line = log_count(m, "stored claim is 31 bytes");
        printf("au4 a 31-byte blob at boot: claimed=%d, log line 'stored claim is 31 bytes'=%d, 'AUTH claimed' lines=%d\n", c6_auth_claimed(), line, log_count(m, "AUTH claimed"));
        CHECK(!c6_auth_claimed() && line == 1 && log_count(m, "AUTH claimed") == 0);
        reset_case(); uint8_t b32[32]; memset(b32, 9, 32); fs_put("c6claim", "sha", 4, 0, b32, 32); au_boot();
        CHECK(c6_auth_claimed());
    }
    verdict("au4", "only the identical digest passes; the compare reads all 32 bytes whatever differs (byte 0, byte 31, every byte, none); a 31-byte stored blob loads as unclaimed with one log line");

    /* au5. the claim window on fake time */
    {   reset_case(); g_auto_bearer = 0; s_claimed = false; c6_auth_start();
        int o0 = c6_auth_window_open(), l0 = (s_active >> C6_LED_UNCLAIMED) & 1; fake_advance(599000000); int o1 = c6_auth_window_open(), l1 = (s_active >> C6_LED_UNCLAIMED) & 1; fake_advance(1000000);
        int o2 = c6_auth_window_open(), l2 = (s_active >> C6_LED_UNCLAIMED) & 1;
        printf("au5 power-up: open %d, at 599 s %d, at 600 s %d; UNCLAIMED LED %d %d %d (it follows the claim, not the window)\n", o0, o1, o2, l0, l1, l2);
        CHECK(o0 && o1 && !o2 && l0 && l1 && l2);
        reset_case(); g_auto_bearer = 0; s_claimed = false; c6_auth_start(); fake_advance(400000000); button_action(C6_BUTTON_PRESS); fake_advance(599900000);
        int p1 = c6_auth_window_open(); fake_advance(100000); int p2 = c6_auth_window_open(), pl = (s_active >> C6_LED_UNCLAIMED) & 1;
        printf("au5 press at 400 s: open at 999.9 s=%d, at 1000 s=%d, UNCLAIMED LED=%d (still unclaimed)\n", p1, p2, pl);
        CHECK(p1 && !p2 && pl);
        reset_case(); uint8_t d[32]; au_digest(G_TOKEN, d); fs_put("c6claim", "sha", 4, 0, d, 32); au_boot(); g_auto_bearer = 1;
        int claimed_led = (s_active >> C6_LED_UNCLAIMED) & 1, claimed_open = c6_auth_window_open(); fake_advance(900000000);
        httpd_req_t r; req_init(&r); api_call(api_release_post, &r);
        int r1 = c6_auth_window_open(), rl = (s_active >> C6_LED_UNCLAIMED) & 1; fake_advance(599900000); int r2 = c6_auth_window_open(); fake_advance(100000); int r3 = c6_auth_window_open(), rl3 = (s_active >> C6_LED_UNCLAIMED) & 1;
        printf("au5 claimed at power-up: open=%d LED=%d (the window is open but the LED is for 'unclaimed'); release at 900 s: http=%d open=%d LED=%d, +599.9 s open=%d, +600 s open=%d LED=%d\n", claimed_open, claimed_led, r.code, r1, rl, r2, r3, rl3);
        CHECK(claimed_open && !claimed_led && r.code == 200 && r1 && rl && r2 && !r3 && rl3);
    }
    verdict("au5", "the window: open from power-up to 600 s; a press at 400 s extends it to 1000 s; a release at 900 s re-opens it 600 s from then; the UNCLAIMED LED source follows the claim alone: set while unclaimed (the window closing does not clear it), clear while claimed");

    /* au6. clear and release */
    {   reset_case(); uint8_t d[32]; au_digest(G_TOKEN, d); fs_put("c6claim", "sha", 4, 0, d, 32); c6_discovery_start(); au_boot();
        int claimed0 = c6_auth_claimed(); fake_advance(700000000); c6_auth_clear();
        int cleared_led = (s_active >> C6_LED_UNCLAIMED) & 1;
        printf("au6 clear: was claimed=%d; key erased=%d claimed=%d UNCLAIMED=%d events_close=%d window open=%d TXT state=%s\n", claimed0, fs_find("c6claim", "sha") < 0, c6_auth_claimed(), cleared_led, g_closes, c6_auth_window_open(), mdns_txt("state"));
        CHECK(claimed0 && fs_find("c6claim", "sha") < 0 && !c6_auth_claimed() && cleared_led && g_closes == 1 && c6_auth_window_open() && !strcmp(mdns_txt("state"), "unclaimed"));
        fake_advance(599000000); int o1 = c6_auth_window_open(); fake_advance(1000000); CHECK(o1 && !c6_auth_window_open());
        httpd_req_t r; req_init(&r); r.h_auth = G_BEARER; api_call(api_log_get, &r); char code[32]; int st = au_code(&r, code, sizeof code);
        printf("au6 the old token afterwards: %d %s\n", st, code);
        CHECK(st == 403 && !strcmp(code, "claim_required"));
        reset_case(); au_state(true, false); c6_discovery_start(); httpd_req_t r1; req_init(&r1); api_call(api_release_post, &r1); st = au_code(&r1, code, sizeof code);
        printf("au6 /release without a token: %d %s; still claimed=%d; closes=%d\n", st, code, c6_auth_claimed(), g_closes);
        CHECK(st == 401 && !strcmp(code, "unauthorized") && c6_auth_claimed() && g_closes == 0);
        httpd_req_t r2; req_init(&r2); r2.h_auth = G_BEARER; api_call(api_release_post, &r2);
        printf("au6 /release with the token: %d %s; claimed=%d UNCLAIMED=%d closes=%d window=%d TXT=%s\n", r2.code, r2.response, c6_auth_claimed(), (s_active >> C6_LED_UNCLAIMED) & 1, g_closes, c6_auth_window_open(), mdns_txt("state"));
        CHECK(r2.code == 200 && !strcmp(r2.response, "{\"ok\":true}") && !c6_auth_claimed() && ((s_active >> C6_LED_UNCLAIMED) & 1) && g_closes == 1 && c6_auth_window_open() && !strcmp(mdns_txt("state"), "unclaimed"));
    }
    verdict("au6", "c6_auth_clear: key erased, unclaimed, the UNCLAIMED LED on, one events_close, window 600 s, TXT unclaimed, the old Bearer 403 claim_required; POST /release needs the token and does the same with 200 {\"ok\":true}");

    /* au7. info, identify, reboot */
    {   const char *states[] = {"open", "closed", "claimed"}; const char *need[] = {"id", "name", "mac", "hw", "fw", "api", "claim", "uptime_s", "ip", "rssi", "partition", "ota_state", "built", "idf"};
        for (int k = 0; k < 3; k++) {
            reset_case(); au_state(k == 2, k == 0); httpd_req_t r; req_init(&r); api_call(api_info_get, &r); cJSON *j = cJSON_Parse(r.response); int all = j != NULL;
            for (size_t f = 0; f < sizeof need / sizeof need[0]; f++) all &= j && cJSON_GetObjectItemCaseSensitive(j, need[f]) != NULL;
            const cJSON *c = j ? cJSON_GetObjectItemCaseSensitive(j, "claim") : NULL;
            int clean = !strstr(r.response, G_TOKEN) && !strstr(r.response, "pw-zq9") && !strstr(r.response, "zq9") && !strstr(r.response, "webhook") && !strstr(r.response, "sha");
            printf("au7 info %s: http=%d claim=%s all %zu fields=%d no secret=%d auth hashes=%d\n", states[k], r.code, c && cJSON_IsString(c) ? c->valuestring : "?", sizeof need / sizeof need[0], all, clean, g_sha_calls);
            CHECK(r.code == 200 && c && !strcmp(c->valuestring, states[k]) && all && clean && g_sha_calls == 0);
            cJSON_Delete(j);
        }
        const char *qs[] = {"0", "1", "60", "99", NULL}; const int wantsec[] = {1, 1, 60, 60, 5};
        for (int k = 0; k < 5; k++) {
            reset_case(); g_auto_bearer = 0; s_claimed = false; c6_auth_start(); httpd_req_t r; req_init(&r); r.s = qs[k]; api_call(api_identify_post, &r);
            int64_t due = timer_in_us("identify"); int win = (s_active >> C6_LED_UNCLAIMED) & 1; int shown = led_winner(s_active) == C6_LED_IDENTIFY;
            fake_advance(due); int after = (s_active >> C6_LED_IDENTIFY) & 1;
            printf("au7 identify s=%s: http=%d %s; timer %lld s (want %d); UNCLAIMED LED set=%d, IDENTIFY shown=%d; cleared by the timer=%d\n", qs[k] ? qs[k] : "(absent)", r.code, r.response, (long long)(due / 1000000), wantsec[k], win, shown, !after);
            CHECK(r.code == 200 && due == (int64_t)wantsec[k] * 1000000 && win && shown && !after);
        }
        reset_case(); au_state(true, false); httpd_req_t rr; req_init(&rr); api_call(api_reboot_post, &rr); char code[32]; int st = au_code(&rr, code, sizeof code);
        reset_case(); au_state(false, true); httpd_req_t ru; req_init(&ru); api_call(api_reboot_post, &ru); char code2[32]; int st2 = au_code(&ru, code2, sizeof code2);
        printf("au7 reboot without the token: claimed %d %s, unclaimed %d %s; tasks=%d restarts=%d\n", st, code, st2, code2, g_ntasks, g_restarts);
        CHECK(st == 401 && st2 == 403 && g_ntasks == 0 && g_restarts == 0);
        reset_case(); g_auto_bearer = 1; au_state(true, false); g_auto_bearer = 1; httpd_req_t rg; req_init(&rg); api_call(api_reboot_post, &rg);
        int t = -1; for (int i = 0; i < g_ntasks; i++) if (!strcmp(g_tasks[i].name, "reboot")) t = i;
        printf("au7 reboot with the token: http=%d %s; reboot tasks=%d, stack %u, events_close before the task was made=%d; restarts so far=%d\n", rg.code, rg.response, tasks_named("reboot"), task_stack("reboot"), t >= 0 ? g_tasks[t].closes_at : -1, g_restarts);
        CHECK(rg.code == 200 && t >= 0 && tasks_named("reboot") == 1 && g_tasks[t].closes_at == 1 && g_restarts == 0 && task_stack("reboot") == 6144);
        int64_t t0 = fake_now; g_tasks[t].fn(NULL);
        printf("au7 the reboot task: waited %lld ms, restarts=%d, events_close total=%d\n", (long long)((fake_now - t0) / 1000), g_restarts, g_closes);
        CHECK(fake_now - t0 == (int64_t)REBOOT_DELAY_MS * 1000 && g_restarts == 1 && g_closes == 2);
    }
    verdict("au7", "/info in the three claim states (every field, no secret, no auth); identify clamps to 1-60 (default 5) and shows over the UNCLAIMED LED; reboot needs the token, answers 200, ends the stream, then a reboot task that waits REBOOT_DELAY_MS (2 s)");

    /* au8. /log */
    {   reset_case(); g_auto_bearer = 1; char chunk[1000]; unsigned long long idx = 0;
        while (s_ring_total < 20000) { for (int i = 0; i < 1000; i++) chunk[i] = (char)('A' + (idx++ % 23)); ring_copy_in(chunk, 1000); }
        uint32_t total = s_ring_total, oldest = total - 16384; char q[32]; int allok = 1;
        struct {const char *name; uint32_t from; uint32_t first; int len;} cs[] = {{"inside", total - 100, total - 100, 100}, {"below the oldest", 5, oldest, 16384}, {"above the total", total + 50, oldest, 16384}, {"zero", 0, oldest, 16384}};
        for (int k = 0; k < 4; k++) {
            httpd_req_t r; req_init(&r); snprintf(q, sizeof q, "from=%u", cs[k].from); r.query = q; api_call(api_log_get, &r);
            char next[16]; snprintf(next, sizeof next, "%u", total); int match = 1;
            for (int i = 0; i < 80 && i < r.resp_len; i++) match &= r.response[i] == (char)('A' + ((cs[k].first + (uint32_t)i) % 23));
            int ok = r.code == 200 && r.resp_len == cs[k].len && au_hdr(&r, "X-Log-Next") && !strcmp(au_hdr(&r, "X-Log-Next"), next) && !strcmp(r.type, "text/plain") && match;
            printf("au8 from %s: http=%d, %d bytes (want %d), X-Log-Next=%s (total %u), type=%s, content from absolute offset %u=%d\n", cs[k].name, r.code, r.resp_len, cs[k].len, au_hdr(&r, "X-Log-Next"), total, r.type, cs[k].first, match);
            allok &= ok;
        }
        CHECK(allok);
        reset_case(); au_state(true, false); httpd_req_t r1; req_init(&r1); api_call(api_log_get, &r1); char code[32]; int st = au_code(&r1, code, sizeof code);
        reset_case(); au_state(false, true); httpd_req_t r2; req_init(&r2); api_call(api_log_get, &r2); char code2[32]; int st2 = au_code(&r2, code2, sizeof code2);
        printf("au8 refused: claimed without the token %d %s; unclaimed %d %s\n", st, code, st2, code2);
        CHECK(st == 401 && !strcmp(code, "unauthorized") && st2 == 403 && !strcmp(code2, "claim_required") && !strcmp(r1.type, "application/json"));
        /* out of memory: /api/v1/log answers the JSON 500 internal; the old /log keeps httpd's plain 500 */
        reset_case(); g_auto_bearer = 1; httpd_req_t m1; req_init(&m1); g_malloc_fail_size = LOG_RING; api_call(api_log_get, &m1); g_malloc_fail_size = 0;
        char code3[32]; int st3 = au_code(&m1, code3, sizeof code3);
        httpd_req_t m2; req_init(&m2); g_malloc_fail_size = LOG_RING; log_get(&m2); g_malloc_fail_size = 0;
        printf("au8 out of memory: /api/v1/log %d %s type=%s; old /log %d '%s'\n", st3, code3, m1.type, m2.code, m2.response);
        CHECK(st3 == 500 && !strcmp(code3, "internal") && !strcmp(m1.type, "application/json") && m2.code == 500 && !strcmp(m2.response, "500 Internal Server Error"));
    }
    verdict("au8", "GET /api/v1/log: token class; X-Log-Next is the total; from below the oldest or above the total gives the oldest; text/plain; refusals are JSON, out of memory included (500 internal); the old /log keeps the plain 500");
}
