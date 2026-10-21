/* pt1-pt4: portal.c (the setup page and the captive-portal answers, called through the handlers portal_register recorded) and dns.c (the catch-all DNS task). */
static httpd_req_t pt_r;
static int pt_call(const char *path, int method, const char *body) {                 /* the handler registered for path + method, or -1 */
    req_init(&pt_r); if (body) req_body(&pt_r, body);
    for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].uri, path) && g_reg[i].method == method) { g_reg[i].handler(&pt_r); return pt_r.code; }
    return -1;
}
static void pt_boot_with(const char *stored_ssid) { reset_case(); g_auto_bearer = 0; if (stored_ssid) wf_store(stored_ssid, WF_PASS); c6_wifi_start(); portal_register((httpd_handle_t)1); }
static void pt_boot(void) { pt_boot_with(NULL); }
static void pt_boot_dns(void) { pt_boot(); s_ap_up = false; }
static void pt_aps(void) {
    snprintf((char *)g_w_aps[0].ssid, 33, "zq9-a"); g_w_aps[0].rssi = -40; g_w_aps[0].authmode = WIFI_AUTH_WPA2_PSK;
    snprintf((char *)g_w_aps[1].ssid, 33, "zq9-b"); g_w_aps[1].rssi = -70; g_w_aps[1].authmode = WIFI_AUTH_OPEN;
    snprintf((char *)g_w_aps[2].ssid, 33, "zq9-c"); g_w_aps[2].rssi = -55; g_w_aps[2].authmode = WIFI_AUTH_WPA2_PSK; g_w_nap = 3;
}
static int pt_count(const char *hay, const char *needle) { return str_count(hay, needle); }
static const char *pt_loc(void) { return resp_hdr(&pt_r, "Location"); }

/* dns.c */
static size_t dq(uint8_t *o, uint16_t id, const char *name, uint16_t type, uint16_t cls) {      /* a standard query, one question */
    size_t n = 0; o[n++] = (uint8_t)(id >> 8); o[n++] = (uint8_t)id; o[n++] = 0x01; o[n++] = 0; o[n++] = 0; o[n++] = 1; memset(o + n, 0, 6); n += 6;
    while (*name) { const char *dot = strchr(name, '.'); size_t l = dot ? (size_t)(dot - name) : strlen(name); o[n++] = (uint8_t)l; memcpy(o + n, name, l); n += l; name += l + (dot ? 1 : 0); }
    o[n++] = 0; o[n++] = (uint8_t)(type >> 8); o[n++] = (uint8_t)type; o[n++] = (uint8_t)(cls >> 8); o[n++] = (uint8_t)cls; return n;
}
static void pt_dns_down(void) { if (s_ap_cb) s_ap_cb(false); }
static int pt_dns_up_again_done;
static void pt_dns_up_then_down(void) { if (!pt_dns_up_again_done) { pt_dns_up_again_done = 1; s_ap_cb(true); } else s_ap_cb(false); }
static void pt_dns_run(int wakes) { g_notify_pass = wakes; if (!setjmp(loop_done)) dns_task(NULL); g_notify_pass = 0; }
static int dr_is_answer(int k, const uint8_t *q, size_t qn) {                          /* reply k answers query q (qn bytes) with 192.168.44.1, TTL 0 */
    static const uint8_t rr[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 44, 1};
    const uint8_t *r = g_lw_sent[k].b; return g_lw_sent[k].n == (int)qn + 16 && r[0] == q[0] && r[1] == q[1] && (r[2] & 0x80) && (r[2] & 0x04) && (r[2] & 0x01) == (q[2] & 0x01) && (r[2] & 0x78) == 0 && r[3] == 0
        && r[4] == 0 && r[5] == 1 && r[6] == 0 && r[7] == 1 && r[8] == 0 && r[9] == 0 && r[10] == 0 && r[11] == 0 && !memcmp(r + 12, q + 12, qn - 12) && !memcmp(r + qn, rr, 16);
}

static void probe_pt(void) {
    puts("==== pt: the setup page and the catch-all DNS ====");
    /* pt1. GET /setup */
    {   pt_boot_with("zq9net"); pt_aps(); size_t m = g_loglen; int c = pt_call("/setup", HTTP_GET, NULL); const char *h = pt_r.response;
        const char *sel = strstr(h, "<select"), *last = NULL; for (const char *p = h; (p = strstr(p, "<option")) != NULL; p++) last = p;
        const char *passin = strstr(h, "name=\"pass\""); char tag[200] = ""; if (passin) { const char *s = passin; while (s > h && *s != '<') s--; const char *e = strchr(passin, '>'); if (e && (size_t)(e - s) < sizeof tag) { memcpy(tag, s, (size_t)(e - s + 1)); tag[e - s + 1] = 0; } }
        printf("pt1 GET /setup (stored zq9net, 3 scanned): http=%d type=%s bytes=%zu name shown=%d current ssid shown=%d options=%d (3 networks + other…) last option is other…=%d free ssid field=%d password field type=%d with a value=%d\n",
               c, pt_r.type, strlen(h), strstr(h, "<h1>Zigbee Anthema 112233</h1>") != NULL, strstr(h, "Current network: <b>zq9net</b>") != NULL, pt_count(h, "<option"), last && strstr(last, ">other…</option>") != NULL, strstr(h, "name=\"ssid\"") != NULL, strstr(tag, "type=\"password\"") != NULL, strstr(tag, "value=") != NULL);
        CHECK(c == 200 && !strcmp(pt_r.type, "text/html; charset=utf-8") && strstr(h, "<h1>Zigbee Anthema 112233</h1>") && strstr(h, "Current network: <b>zq9net</b>") && sel && pt_count(h, "<option") == 4
              && strstr(h, "<option value=\"zq9-a\">zq9-a</option>") && strstr(h, "<option value=\"zq9-b\">zq9-b</option>") && strstr(h, "<option value=\"zq9-c\">zq9-c</option>") && last && strstr(last, "<option value=\"\">other…</option>")
              && strstr(h, "<input name=\"ssid\"") && passin && strstr(tag, "type=\"password\"") && !strstr(tag, "value=") && strstr(h, "method=\"post\" action=\"/setup\""));
        printf("pt1 page: <script>=%d src=%d href=%d any http(s) URL=%d, the password (pw-zq9) in the page=%d or the log=%d; a Content-Length-free chunked answer ended=%d\n", pt_count(h, "<script"), pt_count(h, "src="), pt_count(h, "href="), pt_count(h, "http"), strstr(h, "pw-zq9") != NULL, log_count(m, "pw-zq9"), pt_r.code == 200);
        CHECK(!strstr(h, "<script") && !strstr(h, "src=") && !strstr(h, "href=") && !strstr(h, "http") && !strstr(h, "pw-zq9") && log_count(m, "pw-zq9") == 0);
        pt_boot(); g_w_nap = 0; c = pt_call("/setup", HTTP_GET, NULL); h = pt_r.response;
        printf("pt1 nothing stored, a scan that finds nothing: http=%d 'No network is stored'=%d options=%d (only other…) free field=%d\n", c, strstr(h, "No network is stored.") != NULL, pt_count(h, "<option"), strstr(h, "name=\"ssid\"") != NULL);
        CHECK(c == 200 && strstr(h, "No network is stored.") && pt_count(h, "<option") == 1 && strstr(h, "<option value=\"\">other…</option>") && strstr(h, "name=\"ssid\"") && strstr(h, "name=\"pass\""));
        pt_boot(); g_w_scan_err = ESP_ERR_INVALID_STATE; c = pt_call("/setup", HTTP_GET, NULL); h = pt_r.response;
        printf("pt1 the scan cannot run: http=%d, no select=%d, free ssid field=%d\n", c, strstr(h, "<select") == NULL, strstr(h, "name=\"ssid\"") != NULL);
        CHECK(c == 200 && !strstr(h, "<select") && strstr(h, "name=\"ssid\"") && strstr(h, "name=\"pass\""));
        reset_case(); g_auto_bearer = 0; kv_put_str("name", "zq9 <b>&\"'"); wf_store("a<i>&b", WF_PASS); c6_wifi_start(); portal_register((httpd_handle_t)1); snprintf((char *)g_w_aps[0].ssid, 33, "n<x>&\"'"); g_w_aps[0].rssi = -40; g_w_nap = 1; c = pt_call("/setup", HTTP_GET, NULL); h = pt_r.response;
        printf("pt1 escaping: name, stored ssid and a scanned ssid containing < > & \" ' : raw '<b>'=%d '<i>'=%d '<x>'=%d, escaped name=%d stored=%d scanned=%d\n", strstr(h, "<b>&") != NULL, strstr(h, "<i>") != NULL, strstr(h, "<x>") != NULL,
               strstr(h, "zq9 &lt;b&gt;&amp;&quot;&#39;") != NULL, strstr(h, "a&lt;i&gt;&amp;b") != NULL, strstr(h, "n&lt;x&gt;&amp;&quot;&#39;") != NULL);
        CHECK(!strstr(h, "<b>&") && !strstr(h, "<i>") && !strstr(h, "<x>") && strstr(h, "zq9 &lt;b&gt;&amp;&quot;&#39;") && strstr(h, "a&lt;i&gt;&amp;b") && strstr(h, "n&lt;x&gt;&amp;&quot;&#39;"));
    }
    verdict("pt1", "GET /setup: 200 HTML with the board's name and the stored SSID, a select of the scanned networks with 'other…' last, a free ssid field and a password field without a value=; no script, no src or href, no URL, the password nowhere; every name and SSID HTML-escaped; with no scan the list is left out");

    /* pt2. POST /setup */
    {   pt_boot(); size_t m = g_loglen; int c = pt_call("/setup", HTTP_POST, "ssid=zq9net&pass=pw-zq9-net");
        printf("pt2 ssid=zq9net&pass=<made-up>: http=%d (no credential sent: claimed=%d), station pair as given=%d sets=%d, stored ssid=%s, answer names the ssid=%d, answer carries the password=%d, log carries the password=%d\n", c, (int)s_claimed, w_sta_is("zq9net", WF_PASS), g_w_sta_sets, fs_str("c6wifi", "ssid") ? fs_str("c6wifi", "ssid") : "(none)", strstr(pt_r.response, "<b>zq9net</b>") != NULL, strstr(pt_r.response, "pw-zq9") != NULL, log_count(m, "pw-zq9"));
        CHECK(c == 200 && w_sta_is("zq9net", WF_PASS) && g_w_sta_sets == 1 && fs_str("c6wifi", "ssid") && !strcmp(fs_str("c6wifi", "ssid"), "zq9net") && strstr(pt_r.response, "Saved") && strstr(pt_r.response, "<b>zq9net</b>") && !strstr(pt_r.response, "pw-zq9") && log_count(m, "pw-zq9") == 0 && s_claimed);
        pt_boot(); c = pt_call("/setup", HTTP_POST, "ssid=zq9+net%20x%26y&pass=pw-zq9+net%26");
        printf("pt2 '+', '%%20' and '%%26': http=%d, station pair decoded=%d, answer shows the ssid escaped=%d, password in the answer=%d\n", c, w_sta_is("zq9 net x&y", "pw-zq9 net&"), strstr(pt_r.response, "<b>zq9 net x&amp;y</b>") != NULL, strstr(pt_r.response, "pw-zq9") != NULL);
        CHECK(c == 200 && w_sta_is("zq9 net x&y", "pw-zq9 net&") && strstr(pt_r.response, "<b>zq9 net x&amp;y</b>") && !strstr(pt_r.response, "pw-zq9"));
        pt_boot(); au_state(false, false); c = pt_call("/setup", HTTP_POST, "pick=zq9-a&ssid=&pass="); printf("pt2 unclaimed, pick=zq9-a, empty ssid, empty pass: http=%d pair=%d\n", c, w_sta_is("zq9-a", "")); CHECK(c == 200 && w_sta_is("zq9-a", ""));
        pt_boot(); c = pt_call("/setup", HTTP_POST, "pick=zq9-a&ssid=zq9-text&pass="); printf("pt2 both pick and ssid: the text field wins=%d\n", w_sta_is("zq9-text", "")); CHECK(w_sta_is("zq9-text", ""));
        static char big[400]; memset(big, 'a', 257); big[257] = 0; memcpy(big, "ssid=zq9net&pass=pw-zq9-net&x=", 30);
        char b256[300]; memset(b256, 'a', 256); b256[256] = 0; memcpy(b256, "ssid=zq9net&pass=pw-zq9-net&x=", 30);
        const struct {const char *what; const char *body; const char *reason;} bad[] = {
            {"257-byte body", big, "the form is longer than 256 bytes."}, {"no ssid", "pass=pw-zq9-net", "enter or pick a network name."}, {"empty ssid", "ssid=&pass=pw-zq9-net", "enter or pick a network name."},
            {"5-character password", "ssid=zq9net&pass=ab-q9", "the password empty or 8 to 63"}, {"%0A in the ssid", "ssid=zq9%0Anet&pass=pw-zq9-net", "no control characters"},
            {"a bad escape", "ssid=zq9%zznet&pass=pw-zq9-net", "the network name must be 1 to 32 characters."}, {"an escaped NUL", "ssid=zq9%00net&pass=pw-zq9-net", "the network name must be 1 to 32 characters."},
            {"a 33-byte ssid", "ssid=012345678901234567890123456789012&pass=pw-zq9-net", "the network name must be 1 to 32 characters"}, {"a 64-byte password", "ssid=zq9net&pass=0123456789012345678901234567890123456789012345678901234567890123", "the password empty or 8 to 63"},
            {"a 65-byte password", "ssid=zq9net&pass=01234567890123456789012345678901234567890123456789012345678901234", "the password must be empty or 8 to 63 characters."}};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            pt_boot(); size_t m2 = g_loglen; int s0 = g_w_sta_sets, k0 = g_w_connects, f0 = g_fs_sets; g_w_nap = 0; c = pt_call("/setup", HTTP_POST, bad[i].body); const char *h = pt_r.response;
            printf("pt2 %s: http=%d the form again=%d reason shown=%d, station sets +%d, connects +%d, stored +%d, password in the page=%d in the log=%d\n", bad[i].what, c, strstr(h, "<form") != NULL, strstr(h, bad[i].reason) != NULL, g_w_sta_sets - s0, g_w_connects - k0, g_fs_sets - f0, strstr(h, "pw-zq9") != NULL, log_count(m2, "pw-zq9"));
            CHECK(c == 200 && strstr(h, "<form") && strstr(h, "Not saved: ") && strstr(h, bad[i].reason) && g_w_sta_sets == s0 && g_w_connects == k0 && g_fs_sets == f0 && !fs_str("c6wifi", "ssid") && !strstr(h, "pw-zq9") && !strstr(h, "012345678901234567890123456789012345678901234567890123456789") && log_count(m2, "pw-zq9") == 0);
        }
        pt_boot(); c = pt_call("/setup", HTTP_POST, b256); printf("pt2 a 256-byte body is read (not refused for its size): http=%d reason 'longer than 256' shown=%d\n", c, strstr(pt_r.response, "longer than 256") != NULL); CHECK(c == 200 && !strstr(pt_r.response, "longer than 256"));
        /* with the hotspot down nothing is stored or applied; 403 with the form and the reason; after the failure timer opens the hotspot it works again */
        pt_boot_with("zq9old"); size_t m3 = g_loglen; int ap0 = c6_wifi_ap_up(), s3 = g_w_sta_sets, k3 = g_w_connects, f3 = g_fs_sets, d3 = g_w_disconnects;
        c = pt_call("/setup", HTTP_POST, "ssid=zq9net&pass=pw-zq9-net");
        printf("pt2 hotspot down (stored pair, station connecting): up=%d http=%d, the form again=%d, reason shown=%d, station sets +%d, connects +%d, disconnects +%d, stored +%d, stored ssid=%s, password in the page=%d in the log=%d\n",
               ap0, c, strstr(pt_r.response, "<form") != NULL, strstr(pt_r.response, "Saving works only while the hotspot is open: hold the button 3–9 s.") != NULL, g_w_sta_sets - s3, g_w_connects - k3, g_w_disconnects - d3,
               g_fs_sets - f3, fs_str("c6wifi", "ssid") ? fs_str("c6wifi", "ssid") : "(none)", strstr(pt_r.response, "pw-zq9") != NULL, log_count(m3, "pw-zq9"));
        CHECK(!ap0 && c == 403 && strstr(pt_r.response, "<form") && strstr(pt_r.response, "Not saved: ") && strstr(pt_r.response, "Saving works only while the hotspot is open: hold the button 3–9 s.") && g_w_sta_sets == s3 && g_w_connects == k3 && g_w_disconnects == d3
              && g_fs_sets == f3 && !strcmp(fs_str("c6wifi", "ssid"), "zq9old") && !strstr(pt_r.response, "pw-zq9") && log_count(m3, "pw-zq9") == 0);
        c = pt_call("/setup", HTTP_GET, NULL); printf("pt2 hotspot down: GET /setup still served: http=%d form=%d\n", c, strstr(pt_r.response, "<form") != NULL); CHECK(c == 200 && strstr(pt_r.response, "<form"));
        fake_advance(61000000); int ap1 = c6_wifi_ap_up(); c = pt_call("/setup", HTTP_POST, "ssid=zq9net&pass=pw-zq9-net");
        printf("pt2 after the failure timer opened the hotspot: up=%d http=%d, stored ssid=%s\n", ap1, c, fs_str("c6wifi", "ssid") ? fs_str("c6wifi", "ssid") : "(none)");
        CHECK(ap1 && c == 200 && strstr(pt_r.response, "Saved") && !strcmp(fs_str("c6wifi", "ssid"), "zq9net"));
        pt_boot(); req_init(&pt_r); req_body(&pt_r, "ssid=zq9net&pass=pw-zq9-net"); pt_r.recv_fail = 1; int rc = -1; for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].uri, "/setup") && g_reg[i].method == HTTP_POST) rc = g_reg[i].handler(&pt_r);
        printf("pt2 the body cannot be received: handler returns %d (a failure), station sets=%d\n", rc, g_w_sta_sets); CHECK(rc != 0 && g_w_sta_sets == 0);
    }
    verdict("pt2", "POST /setup: the decoded ssid and password reach c6_wifi_set (+, %20, %26 decoded; pick used when the text field is empty); the answer names the SSID escaped and never the password, with no credential asked; a 257-byte body, no or an empty ssid, a 5-character or 64-character password, %0A or %00 or a bad escape in the ssid, a 33-byte ssid: the form again with a reason, c6_wifi_set not applied, nothing stored; with the hotspot down: 403 with the form and the reason, nothing stored or applied, GET /setup still served");

    /* pt3. the probe paths and the registry */
    {   const char *paths[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt"};
        pt_boot(); int reg = g_nreg, dup = 0, get = 0, post = 0;
        for (int i = 0; i < g_nreg; i++) { for (int j = 0; j < i; j++) dup += !strcmp(g_reg[i].uri, g_reg[j].uri) && g_reg[i].method == g_reg[j].method; get += g_reg[i].method == HTTP_GET; post += g_reg[i].method == HTTP_POST; }
        int have_all = 1; for (int i = 0; i < 9; i++) { int f = 0; for (int j = 0; j < g_nreg; j++) f |= !strcmp(g_reg[j].uri, paths[i]) && g_reg[j].method == HTTP_GET; have_all &= f; }
        int setup_get = 0, setup_post = 0, root = 0; for (int j = 0; j < g_nreg; j++) { setup_get |= !strcmp(g_reg[j].uri, "/setup") && g_reg[j].method == HTTP_GET; setup_post |= !strcmp(g_reg[j].uri, "/setup") && g_reg[j].method == HTTP_POST; root |= !strcmp(g_reg[j].uri, "/") && g_reg[j].method == HTTP_GET; }
        printf("pt3 portal handlers registered: %d (GET %d, POST %d): /setup GET=%d POST=%d, / GET=%d, the nine probe paths=%d; registered twice=%d\n", reg, get, post, setup_get, setup_post, root, have_all, dup);
        CHECK(reg == 12 && get == 11 && post == 1 && dup == 0 && have_all && setup_get && setup_post && root);
        int bad_up = 0, bad_down = 0, ok_setup = 0;
        pt_boot(); s_ap_up = true;
        for (int i = 0; i < 9; i++) { int c = pt_call(paths[i], HTTP_GET, NULL); const char *l = pt_loc(); if (c != 302 || !l || strcmp(l, "http://192.168.44.1/setup")) { bad_up++; printf("  pt3 UP WRONG %s: http=%d Location=%s\n", paths[i], c, l ? l : "(none)"); } }
        int c = pt_call("/", HTTP_GET, NULL); int root_up = c == 302 && pt_loc() && !strcmp(pt_loc(), "/setup"); c = pt_call("/setup", HTTP_GET, NULL); ok_setup += c == 200;
        s_ap_up = false;
        for (int i = 0; i < 9; i++) { c = pt_call(paths[i], HTTP_GET, NULL); if (c != 404 || pt_loc()) { bad_down++; printf("  pt3 DOWN WRONG %s: http=%d\n", paths[i], c); } }
        c = pt_call("/", HTTP_GET, NULL); int root_down = c == 302 && pt_loc() && !strcmp(pt_loc(), "/setup"); c = pt_call("/setup", HTTP_GET, NULL); ok_setup += c == 200;
        printf("pt3 AP up: the nine probe paths 302 to http://192.168.44.1/setup, wrong=%d; AP down: the nine are 404, wrong=%d; GET / is 302 /setup both ways (up %d, down %d); /setup served in both=%d of 2\n", bad_up, bad_down, root_up, root_down, ok_setup);
        CHECK(bad_up == 0 && bad_down == 0 && root_up && root_down && ok_setup == 2);
    }
    verdict("pt3", "the nine captive-portal probe paths are 302 to http://192.168.44.1/setup while the hotspot is up and 404 when it is down; GET / is 302 /setup both ways (always); /setup is served in both; exactly 12 handlers are registered (/setup GET and POST, nine probes, /), none twice");

    /* pt4. the catch-all DNS */
    {   uint8_t q1[100], q2[100], q3[100], q4[100], hdr[12], junk[600], resp[60], op[60], two[100], ptr[60]; size_t n1 = dq(q1, 0x1234, "example.com", 1, 1), n2 = dq(q2, 0x5678, "connectivitycheck.gstatic.com", 1, 1), n3 = dq(q3, 0x9ABC, "zq9.test", 1, 1), n4 = dq(q4, 0x0042, "example.com", 28, 1);
        memcpy(hdr, q1, 12); memset(junk, 0xFF, sizeof junk); size_t nr = dq(resp, 1, "a.b", 1, 1); resp[2] |= 0x80; size_t no = dq(op, 2, "a.b", 1, 1); op[2] = 0x09; size_t nt = dq(two, 3, "a.b", 1, 1); two[5] = 2;
        size_t np = dq(ptr, 4, "a.b", 1, 1); ptr[12] = 0xC0; uint8_t chaos[100]; size_t nc = dq(chaos, 5, "a.b", 1, 3);
        pt_boot_dns(); dns_start(); dns_start(); int tasks = tasks_named("dns"); unsigned stack = task_stack("dns"); s_ap_cb(true); s_ap_cb(true); int notes = notify_count(s_dns_task);
        lw_queue(q1, (int)n1); lw_queue(q2, (int)n2); lw_queue(q3, (int)n3); lw_queue(q4, (int)n4); lw_queue(hdr, 12); lw_queue(junk, 600); lw_queue(resp, (int)nr); lw_queue(op, (int)no); lw_queue(two, (int)nt); lw_queue(ptr, (int)np); lw_queue(q1, 16);
        g_lw_empty = pt_dns_down; pt_dns_run(2);
        printf("pt4 dns_start twice, up twice: tasks=%d (stack %u), notifications=%d; sockets opened=%d (bound %u.%u.%u.%u:%u, receive timeout %d s), closed=%d; replies=%d\n", tasks, stack, notes, g_lw_sockets, g_lw_ip[0], g_lw_ip[1], g_lw_ip[2], g_lw_ip[3], g_lw_port, g_lw_rcvtimeo, g_lw_closes, g_lw_nsent);
        CHECK(tasks == 1 && stack == 4096 && notes == 2 && g_lw_sockets == 1 && g_lw_binds == 1 && g_lw_port == 53 && g_lw_ip[0] == 192 && g_lw_ip[1] == 168 && g_lw_ip[2] == 44 && g_lw_ip[3] == 1 && g_lw_rcvtimeo == 1 && g_lw_closes == 1 && g_lw_nsent == 4);
        int a1 = g_lw_nsent > 0 && dr_is_answer(0, q1, n1), a2 = g_lw_nsent > 1 && dr_is_answer(1, q2, n2), a3 = g_lw_nsent > 2 && dr_is_answer(2, q3, n3);
        int aaaa = g_lw_nsent > 3 && g_lw_sent[3].n == (int)n4 && g_lw_sent[3].b[0] == 0 && g_lw_sent[3].b[1] == 0x42 && (g_lw_sent[3].b[2] & 0x80) && g_lw_sent[3].b[6] == 0 && g_lw_sent[3].b[7] == 0 && !memcmp(g_lw_sent[3].b + 12, q4 + 12, n4 - 12);
        printf("pt4 three A queries answered with 192.168.44.1, TTL 0, id echoed, QR + AA set, RD copied: %d %d %d; AAAA: the question back with no answer=%d; no reply to a 12-byte header, 600 bytes of garbage, a response, opcode 1, two questions, a name pointer, a truncated question (7 queries, replies total %d of 11)\n", a1, a2, a3, aaaa, g_lw_nsent);
        CHECK(a1 && a2 && a3 && aaaa);
        (void)nc;
        /* up again after down: a new socket; down while a query is pending in the same task */
        g_lw_nsent = 0; lw_queue(q1, (int)n1); lw_queue(chaos, (int)nc); g_lw_qpos = g_lw_qn - 2; pt_dns_up_again_done = 0; s_ap_cb(true); g_lw_empty = pt_dns_up_then_down; g_lw_empties = 0; pt_dns_run(2);
        printf("pt4 up again later: sockets opened in total=%d (closed %d), an up arriving while serving did not open another; replies this time=%d (the class-CHAOS question gets no A answer)\n", g_lw_sockets, g_lw_closes, g_lw_nsent);
        CHECK(g_lw_sockets == 2 && g_lw_closes == 2 && g_lw_nsent == 2 && dr_is_answer(0, q1, n1) && g_lw_sent[1].n == (int)nc);
        int s0 = g_lw_sockets, cl0 = g_lw_closes; g_lw_socket_err = 1; s_ap_cb(true); g_lw_empty = NULL; pt_dns_run(1);
        printf("pt4 the socket cannot be created: sockets +%d, closes +%d, the task left its loop normally\n", g_lw_sockets - s0, g_lw_closes - cl0); CHECK(g_lw_sockets == s0 && g_lw_closes == cl0); g_lw_socket_err = 0;
        s0 = g_lw_sockets; cl0 = g_lw_closes; g_lw_bind_err = 1; s_ap_cb(true); pt_dns_run(1);
        printf("pt4 bind fails: sockets +%d, closed again +%d\n", g_lw_sockets - s0, g_lw_closes - cl0); CHECK(g_lw_sockets == s0 + 1 && g_lw_closes == cl0 + 1); g_lw_bind_err = 0;
        pt_boot_dns(); s_ap_up = true; dns_start(); int want_at_start = s_dns_want; g_lw_empty = pt_dns_down; lw_queue(q3, (int)n3); pt_dns_run(2);
        printf("pt4 the hotspot already up when dns_start runs: serving began=%d, replies=%d, closed=%d\n", want_at_start, g_lw_nsent, g_lw_closes); CHECK(want_at_start && g_lw_nsent == 1 && dr_is_answer(0, q3, n3) && g_lw_closes == 1);
        pt_boot_dns(); dns_start(); g_lw_recv_err = 1; s_ap_cb(true); pt_dns_run(1);
        printf("pt4 a receive error other than the timeout: sockets=%d, closed=%d (closed until the hotspot reopens)\n", g_lw_sockets, g_lw_closes); CHECK(g_lw_sockets == 1 && g_lw_closes == 1);
        pt_boot_dns(); dns_start(); s_ap_cb(true); dns_stop(); CHECK(!s_dns_want);
    }
    verdict("pt4", "dns.c: an A query of three names gets one answer 192.168.44.1, TTL 0, id echoed, QR and AA set; AAAA gets the question back with no answer; a 12-byte header, 600 bytes of garbage, a response, another opcode, two questions, a name pointer or a short question get no reply; dns_start twice and up twice give one task and one socket (192.168.44.1:53, 1 s timeout); down closes it; up again opens a new one; a socket, bind or receive failure closes it and the task goes on");
}
