/* cx1-cx3: the cross-cutting checks, run last. They read what the probes before them left behind (counters, captured answers, captured output) and
 * drive a little more of their own. Order of execution: cx2, cx3, then cx1 (so cx1's counters include what cx2 and cx3 drove). */
static const struct {int code; const char *word;} cx_ok[] = {{400, "bad_request"}, {400, "sha_mismatch"}, {400, "bad_image"}, {401, "unauthorized"}, {403, "claim_required"}, {404, "not_found"}, {409, "already_claimed"}, {409, "window_closed"}, {409, "wrong_role"},
    {409, "no_press"}, {409, "already_joined"}, {409, "rejoining"}, {409, "busy"}, {409, "wrong_firmware"}, {413, "too_large"}, {500, "storage"}, {500, "internal"}, {503, "zigbee_busy"}};       /* protocol section 7 */
static int cx_shape(int code, const char *type, const char *body) {            /* an object with exactly the string keys error and message, as JSON, with a section 7 pair */
    cJSON *j = cJSON_Parse(body); int ok = j && cJSON_IsObject(j) && cJSON_GetArraySize(j) == 2 && !strcmp(type, "application/json");
    const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL, *m = j ? cJSON_GetObjectItemCaseSensitive(j, "message") : NULL;
    ok = ok && e && m && cJSON_IsString(e) && cJSON_IsString(m);
    int pair = 0; for (size_t i = 0; ok && i < sizeof cx_ok / sizeof cx_ok[0]; i++) pair |= cx_ok[i].code == code && !strcmp(cx_ok[i].word, e->valuestring);
    cJSON_Delete(j); return ok && pair;
}
static int cx_bad, cx_leak, cx_n500;
static portMUX_TYPE cx_mux = portMUX_INITIALIZER_UNLOCKED;               /* a second mux, for the nesting detector's control */
static void cx_answer(httpd_req_t *r, const char *what) { if (r->code >= 400 && !cx_shape(r->code, r->type, r->response)) { cx_bad++; printf("  cx2 WRONG SHAPE (%s): %d %s '%.120s'\n", what, r->code, r->type, r->response); } }
static void cx_state(httpd_req_t *r) { req_init(r); api_call(state_get, r); }
static void cx_settings(httpd_req_t *r) { reset_case(); cfg_load(); req_init(r); req_body(r, "{\"name\":\"zq9 light\",\"settle_ms\":2500}"); api_call(settings_put, r); }
static void cx_light(httpd_req_t *r) { lt_fresh(0, CFG_ROLE_MIRROR); req_init(r); req_body(r, "{\"on\":1,\"level\":50,\"hs\":[1,2]}"); api_call(light_v1_post, r); }
static void cx_sweep(const char *name, void (*call)(httpd_req_t *)) {                 /* fail each cJSON allocation of one request in turn */
    httpd_req_t r; reset_case(); long a0 = g_cj_allocs; call(&r); long A = g_cj_allocs - a0; int five = 0, ok200 = 0, leaks = 0, bad0 = cx_bad;
    for (long n = 1; n <= A; n++) {
        reset_case(); long l0 = g_cj_live; g_cj_fail_at = n; call(&r); g_cj_fail_at = 0;
        if (g_cj_live != l0) { leaks++; cx_leak++; printf("  cx2 LEAK (%s): allocation %ld failing leaves %ld cJSON objects live\n", name, n, g_cj_live - l0); }
        cx_answer(&r, name); five += r.code == 500; ok200 += r.code == 200;
    }
    printf("cx2 %s: %ld cJSON allocations in one request; each failing in turn: 500 internal=%d, 200=%d, other refusals=%ld, objects left live=%d, wrong shapes=%d\n", name, A, five, ok200, A - five - ok200, leaks, cx_bad - bad0);
    cx_n500 += five;
}
/* the secrets scan */
static int cx_find(const uint8_t *h, size_t hn, const char *needle) { size_t nl = strlen(needle), c = 0; for (size_t i = 0; i + nl <= hn; i++) if (!memcmp(h + i, needle, nl)) c++; return (int)c; }
static void cx_hex(const char *tok, char *out) { uint8_t d[32]; au_digest(tok, d); for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]); }
static char cx_bearer[80];
static void cx_call(int (*h)(httpd_req_t *), const char *body, httpd_req_t *r) { req_init(r); r->h_auth = cx_bearer; if (body) req_body(r, body); api_call(h, r); }

static void probe_cx(void) {
    puts("==== cx: cross-cutting ====");
    /* cx2. the error shape and no leak */
    {   int n0 = g_nerrs, by[6] = {0}, bad0 = 0; int full = g_nerrs >= 256;
        for (int k = 0; k < g_nerrs; k++) {
            if (!cx_shape(g_errs[k].code, g_errs[k].type, g_errs[k].body)) { bad0++; printf("  cx2 WRONG SHAPE in answer %d: %d %s '%.120s'\n", k, g_errs[k].code, g_errs[k].type, g_errs[k].body); }
            by[g_errs[k].code / 100 - 4 < 6 ? g_errs[k].code / 100 - 4 : 5]++;
        }
        printf("cx2 every 4xx/5xx /api/v1 answer the probes produced: %d captured (capture full=%d); 4xx=%d 5xx=%d; wrong shapes=%d\n", n0, full, by[0], by[1], bad0);
        int codes[24] = {0}; for (int k = 0; k < g_nerrs; k++) for (size_t i = 0; i < sizeof cx_ok / sizeof cx_ok[0]; i++) if (g_errs[k].code == cx_ok[i].code && strstr(g_errs[k].body, cx_ok[i].word)) codes[i]++;
        printf("cx2 by word:"); for (size_t i = 0; i < sizeof cx_ok / sizeof cx_ok[0]; i++) if (codes[i]) printf(" %d/%s=%d", cx_ok[i].code, cx_ok[i].word, codes[i]); puts("");
        int need = 1; const char *must[] = {"bad_request", "unauthorized", "claim_required", "wrong_role", "no_press", "already_claimed", "window_closed", "zigbee_busy"};
        for (size_t m = 0; m < sizeof must / sizeof must[0]; m++) { int seen = 0; for (size_t i = 0; i < sizeof cx_ok / sizeof cx_ok[0]; i++) if (!strcmp(cx_ok[i].word, must[m])) seen += codes[i]; if (!seen) { need = 0; printf("  cx2 no answer with '%s' was captured\n", must[m]); } }
        CHECK(!full && bad0 == 0 && n0 >= 40 && need);
        cx_bad = cx_leak = cx_n500 = 0;
        cx_sweep("GET /state", cx_state); cx_sweep("PUT /settings", cx_settings); cx_sweep("POST /light", cx_light);
        const char *lb[] = {"{\"on\":2}", "{\"on\":1,\"hs\":[[1],2]}", "null", "[1]", "{\"on\":1,\"bri\":1}", "{\"on\":1,\"ct\":300,\"hs\":[1,2]}", "{\"on\":1,\"level\":{\"a\":1}}", "{\"on\":1,\"hs\":[1,2,3]}", "", "{\"on\":"};
        const char *sb[] = {"{\"name\":\"\"}", "{\"role\":\"x\"}", "{\"webhook\":\"nope\"}", "{\"ext_pan\":\"zz\"}", "{\"settle_ms\":1}", "[]", "null", "{\"a\":1}", "{\"a\":{\"b\":1}}", "{\"name\":5,\"settle_ms\":2500}", "{\"settle_ms\":2500,\"settle_ms\":2600}"};
        int refusals = 0;
        for (size_t i = 0; i < sizeof lb / sizeof lb[0]; i++) { lt_fresh(0, CFG_ROLE_MIRROR); long l0 = g_cj_live; lt_post(lb[i]); refusals += lt_r.code >= 400; cx_answer(&lt_r, "light"); if (g_cj_live != l0) { cx_leak++; printf("  cx2 LEAK in /light for '%s'\n", lb[i]); } }
        for (size_t i = 0; i < sizeof sb / sizeof sb[0]; i++) { reset_case(); cfg_load(); long l0 = g_cj_live; bd_put(sb[i]); refusals += bd_req.code >= 400; cx_answer(&bd_req, "settings"); if (g_cj_live != l0) { cx_leak++; printf("  cx2 LEAK in PUT /settings for '%s'\n", sb[i]); } }
        { httpd_req_t r; reset_case(); long l0 = g_cj_live; g_auto_bearer = 0; req_init(&r); api_call(state_get, &r); refusals += r.code >= 400; cx_answer(&r, "state"); if (g_cj_live != l0) cx_leak++; au_state(false, false); req_init(&r); api_call(state_get, &r); refusals += r.code >= 400; cx_answer(&r, "state"); if (g_cj_live != l0) cx_leak++; }
        printf("cx2 allocation-failure sweeps and %d refused bodies: wrong shapes=%d, leaks=%d, 500s from a failing allocation=%d\n", refusals, cx_bad, cx_leak, cx_n500);
        CHECK(cx_bad == 0 && cx_leak == 0 && refusals >= 18 && cx_n500 > 0);
    }
    verdict("cx2", "every 4xx/5xx /api/v1 answer in the run (the probes' own, and a sweep of refused bodies) is an object with exactly the string keys error and message, as application/json, with a protocol section 7 code and word; with the counting hooks, no cJSON object is left live after any error path of /light, PUT /settings and /state, including each allocation failing in turn");

    /* cx3. the secrets scan */
    {   struct {const char *what; const uint8_t *p; size_t n;} hay[4] = {{"log", (const uint8_t *)g_logbuf, g_loglen}, {"JSON/HTML/SSE", (const uint8_t *)g_out_all, g_out_len}, {"Improv", g_usb_all, g_usb_all_len}, {"mDNS", (const uint8_t *)g_mdns_all, g_mdns_all_len}};
        char dg[65]; cx_hex(G_TOKEN, dg);
        const char *pats[] = {"pw-zq9", "tok-zq9", "wrong-zq9", "test_zq9", "c6_gfake", dg};
        const char *pn[] = {"the password", "the made-up claim token", "a wrong token", "the webhook id", "the webhook id (masked-test)", "the claim token's digest hex"};
        int hits = 0; printf("cx3 scanned: %zu log bytes, %zu bytes of JSON/HTML/SSE, %zu Improv bytes, %zu mDNS bytes\n", g_loglen, g_out_len, g_usb_all_len, g_mdns_all_len);
        for (int p = 0; p < 6; p++) for (int h = 0; h < 4; h++) { int c = cx_find(hay[h].p, hay[h].n, pats[p]); if (c) { hits += c; printf("  cx3 LEAK: %s found %d time(s) in the %s\n", pn[p], c, hay[h].what); } }
        printf("cx3 over the whole run (every path above): hits=%d; the ssid 'zq9net' may appear: log=%d\n", hits, cx_find(hay[0].p, hay[0].n, "zq9net"));
        CHECK(hits == 0 && g_loglen > 1000 && g_loglen + 1000 < sizeof g_logbuf && g_out_len > 1000 && g_out_len + 4000 < sizeof g_out_all && g_usb_all_len > 100 && g_mdns_all_len > 50);
        /* a claim with a token the scan knows, then every kind of request */
        reset_case(); for (int i = 0; i < 32; i++) g_rand_seq[i] = (uint8_t)(i * 11 + 5); g_rand_seq_n = 32; g_auto_bearer = 0; s_claimed = false; memset(s_digest, 0, 32); c6_discovery_start(); c6_auth_start(); events_start(); c6_wifi_start();
        char T[65]; for (int i = 0; i < 32; i++) snprintf(T + 2 * i, 3, "%02x", (uint8_t)(i * 11 + 5)); char DT[65]; cx_hex(T, DT);
        httpd_req_t r; req_init(&r); api_call(api_claim_post, &r); int claimed = r.code == 200 && strstr(r.response, T) != NULL && c6_auth_claimed();
        snprintf(cx_bearer, sizeof cx_bearer, "Bearer %s", T); size_t o0 = g_out_len, l0 = g_loglen, u0 = g_usb_all_len, m0 = g_mdns_all_len;
        req_init(&r); api_call(api_info_get, &r); int info_ok = r.code == 200;
        cx_call(settings_put, "{\"webhook\":\"http://192.0.2.49:8123/api/webhook/c6_zq9hook0123456789\",\"name\":\"zq9 light\",\"role\":\"mirror\"}", &r); int put_ok = r.code == 200 && !strstr(r.response, "zq9hook");
        cx_call(settings_get, NULL, &r); int get_masked = r.code == 200 && strstr(r.response, "api/webhook/c6_z\xE2\x80\xA6") != NULL && !strstr(r.response, "zq9hook");
        cx_call(state_get, NULL, &r); int state_ok = r.code == 200; cx_call(light_v1_post, "{\"on\":1}", &r); int light_ok = r.code == 200;
        cx_call(api_log_get, NULL, &r); int log_ok = r.code == 200; cx_call(events_get, NULL, &r); state_mark(ST_LIGHT | ST_BOARD | ST_SETTINGS); events_step(fake_now + 1000);
        { uint8_t p[120], f[160]; size_t pn2 = im_c1(p, "zq9net", 6, 6, WF_PASS, 10, 10), fn = im_rpc(f, 1, p, pn2); im_put(f, fn, 0); im_put_rpc(3, 2000000); im_put_rpc(2, 2000000); im_put_rpc(4, 2000000); im_script(1000000, 1, 0, 0); im_run(40); }
        req_init(&r); req_body(&r, "ssid=zq9net&pass=pw-zq9-net"); g_auto_bearer = 0; s_ap_up = true; portal_register((httpd_handle_t)1); for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].uri, "/setup") && g_reg[i].method == HTTP_POST) g_reg[i].handler(&r);
        req_init(&r); for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].uri, "/setup") && g_reg[i].method == HTTP_GET) g_reg[i].handler(&r);
        static char sbuf[4096]; state_json(sbuf, sizeof sbuf, ST_ALL); c6_discovery_update();
        printf("cx3 a controlled claim: claimed=%d (the token is in its own answer only); then info=%d put=%d get masks the webhook=%d state=%d light=%d log=%d, stream, Improv commands 1-4 with the password, /setup POST and GET; %zu more bytes sent, %zu log bytes, %zu Improv bytes, %zu mDNS bytes\n",
               claimed, info_ok, put_ok, get_masked, state_ok, light_ok, log_ok, g_out_len - o0, g_loglen - l0, g_usb_all_len - u0, g_mdns_all_len - m0);
        CHECK(claimed && info_ok && put_ok && get_masked && state_ok && light_ok && log_ok && g_usb_all_len > u0 && g_out_len > o0 + 500);
        const char *pats2[] = {T, DT, "zq9hook", "pw-zq9"}; const char *pn2[] = {"the claim token", "its digest hex", "the webhook id", "the password"};
        int hits2 = 0;
        struct {const char *what; const uint8_t *p; size_t n;} h2[5] = {{"log", (const uint8_t *)g_logbuf + l0, g_loglen - l0}, {"JSON/HTML/SSE", (const uint8_t *)g_out_all + o0, g_out_len - o0}, {"Improv", g_usb_all + u0, g_usb_all_len - u0},
            {"mDNS", (const uint8_t *)g_mdns_all + m0, g_mdns_all_len - m0}, {"state object", (const uint8_t *)sbuf, strlen(sbuf)}};
        for (int p = 0; p < 4; p++) for (int h = 0; h < 5; h++) { int c = cx_find(h2[h].p, h2[h].n, pats2[p]); if (c) { hits2 += c; printf("  cx3 LEAK: %s found %d time(s) in the %s\n", pn2[p], c, h2[h].what); } }
        printf("cx3 after the claim: the token, its digest, the webhook id and the password in any log line, JSON, HTML, SSE chunk, Improv packet, TXT item or the state object: %d hits\n", hits2);
        CHECK(hits2 == 0);
        /* the ssid and the masked webhook are allowed, and are there */
        printf("cx3 allowed and present: ssid in the log=%d, masked webhook in the settings answer=%d\n", cx_find(h2[0].p, h2[0].n, "zq9net") > 0, get_masked);
    }
    verdict("cx3", "the secrets scan over the whole run and over a controlled claim: the password, the claim token, a wrong token, its digest in hex, and the webhook id appear in no log line, JSON, HTML or SSE byte sent, /info, the state object, Improv packet or mDNS TXT item; the ssid may appear and the webhook only masked");

    /* cx1. the counters */
    {   int n0 = g_nested, c0 = g_call_in_crit, u0 = g_crit_unbalanced, v0 = g_ctx_viol_total + g_ctx_viol;
        printf("cx1 over the whole run (%d probes before this one): nested sections=%d, a stack/HTTP/NVS/Wi-Fi/mDNS/timer/chunk call inside a section or an attribute write without the lock=%d, unbalanced exits=%d, ended inside a section=%d, nvs_* / c6_wifi_set / socket / timer-create call in stack context=%d (notify calls inside a section, information: %d)\n",
               nresults, n0, c0, u0, g_crit != 0, v0, g_notify_in_crit);
        printf("cx1 mutex take of a held / give of a free one / a close leaving the events lock held=%d\n", g_sem_viol);
        CHECK(n0 == 0 && c0 == 0 && u0 == 0 && g_crit == 0 && v0 == 0 && g_sem_viol == 0 && nresults > 50);
        /* the detectors detect: provoke each once, then put the counters back */
        int sn = g_nested, sc = g_call_in_crit, su = g_crit_unbalanced, sv = g_ctx_viol, svt = g_ctx_viol_total, ss = g_crit;
        portENTER_CRITICAL(&s_light_mux); portENTER_CRITICAL(&s_ev_mux); portEXIT_CRITICAL(&s_ev_mux); portEXIT_CRITICAL(&s_light_mux); int nested_fires = g_nested == sn;          /* the one allowed nesting */
        portENTER_CRITICAL(&s_light_mux); portENTER_CRITICAL(&cx_mux); portEXIT_CRITICAL(&cx_mux); portEXIT_CRITICAL(&s_light_mux); nested_fires = nested_fires && g_nested == sn + 1;
        portENTER_CRITICAL(&s_light_mux); esp_timer_handle_t th; esp_timer_create_args_t ta = {.name = "cx"}; esp_timer_create(&ta, &th); portEXIT_CRITICAL(&s_light_mux); int call_fires = g_call_in_crit == sc + 1;
        portEXIT_CRITICAL(&s_light_mux); int unb_fires = g_crit_unbalanced == su + 1;
        g_in_stack_ctx = 1; int h; fs_open("cx", 0, &h); g_in_stack_ctx = 0; int ctx_fires = g_ctx_viol == sv + 1;
        printf("cx1 control: the one allowed nesting (state_mark's mux inside s_light_mux) is not counted, a second kind is=%d; a timer create in a section is counted=%d; an unbalanced exit is counted=%d; an NVS call in stack context is counted=%d\n", nested_fires, call_fires, unb_fires, ctx_fires);
        CHECK(nested_fires && call_fires && unb_fires && ctx_fires);
        g_nested = sn; g_call_in_crit = sc; g_crit_unbalanced = su; g_ctx_viol = sv; g_ctx_viol_total = svt; g_crit = ss; fs_clear();
    }
    verdict("cx1", "after every probe above: critical sections balanced, no nesting except state_mark's own mux inside s_light_mux, no stack/HTTP/NVS/Wi-Fi/mDNS/timer-create/chunk-send call inside a section, no nvs_*, c6_wifi_set, socket or timer-create call while the stack's context flag is set; and each detector is shown to fire");
}
