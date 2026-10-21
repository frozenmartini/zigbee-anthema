/* wf1-wf8: c6_wifi.c (credentials, migration, the failure timer, the hotspot, backoff, scan). Event handlers and timer callbacks run
 * with g_in_stack_ctx set, so an NVS call or c6_wifi_set from one is a counted violation (cx1). */
#define WF_PASS "pw-zq9-net"        /* made-up; it starts with pw-zq9 (6 chars, under the 8-char minimum), which the probes use as a prefix */
static int wf_ap_cb_up = 0, wf_ap_cb_down = 0;
static void wf_ap_cb(bool up) { if (up) wf_ap_cb_up++; else wf_ap_cb_down++; }
static void wf_ev(esp_event_base_t b, int32_t id, void *data) { g_in_stack_ctx = 1; on_event(NULL, b, id, data); g_in_stack_ctx = 0; }
static void wf_got_ip(void) { ip_event_got_ip_t e; memset(&e, 0, sizeof e); esp_netif_set_ip4_addr(&e.ip_info.ip, 192, 0, 2, 77); g_w_connecting = 0; wf_ev(IP_EVENT, IP_EVENT_STA_GOT_IP, &e); }
static void wf_disc(int reason) { wifi_event_sta_disconnected_t d = {.reason = (uint8_t)reason}; g_w_connecting = 0; wf_ev(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d); }
static void wf_store(const char *ssid, const char *pass) { fs_put("c6wifi", "ssid", 3, 0, ssid, strlen(ssid) + 1); fs_put("c6wifi", "pass", 3, 0, pass, strlen(pass) + 1); }
static void wf_boot_stored(void) { reset_case(); wf_ap_cb_up = wf_ap_cb_down = 0; wf_store("zq9net", WF_PASS); c6_wifi_start(); c6_wifi_on_ap(wf_ap_cb); }

static struct {char ssid[34]; int rssi;} res[40]; static int nres, nested_rc, nested_calls;
static void scan_cb(const char *ssid, int rssi, bool secured, void *ctx) {
    (void)secured; (void)ctx;
    if (nres < 40) { snprintf(res[nres].ssid, sizeof res[0].ssid, "%s", ssid); res[nres].rssi = rssi; nres++; }
    if (!nested_calls) { nested_calls++; nested_rc = c6_wifi_scan(scan_cb, NULL); }
}

static void probe_wf(void) {
    puts("==== wf: Wi-Fi core and the hotspot trigger ====");
    /* wf1. three NVS states at boot */
    {   size_t m; int ok = 1;
        wf_boot_stored(); m = 0;
        wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        printf("wf1 (a) stored: sta set=%d pair ok=%d fs sets=%d migrated lines=%d connects=%d state=%d ap=%d\n", g_w_sta_sets, w_sta_is("zq9net", WF_PASS), g_fs_sets, log_count(m, "WIFI credentials migrated"), g_w_connects, (int)c6_wifi_state(), c6_wifi_ap_up());
        (void)m;
        CHECK(g_w_sta_sets == 1 && w_sta_is("zq9net", WF_PASS) && g_fs_sets == 0 && g_fs_commits == 0 && g_w_connects == 1 && c6_wifi_state() == C6_WIFI_CONNECTING && !c6_wifi_ap_up());
        reset_case(); strcpy(g_kconfig_wifi_ssid, "zq9net"); strcpy(g_kconfig_wifi_pass, WF_PASS); m = g_loglen;
        c6_wifi_start();
        int l1 = log_count(m, "WIFI credentials migrated from the build");
        printf("wf1 (b) build pair: sta pair ok=%d stored ssid=%s pass-stored=%d commits=%d sets=%d migration lines=%d\n", w_sta_is("zq9net", WF_PASS), fs_str("c6wifi", "ssid"), fs_str("c6wifi", "pass") != NULL, g_fs_commits, g_fs_sets, l1);
        CHECK(w_sta_is("zq9net", WF_PASS) && fs_str("c6wifi", "ssid") && !strcmp(fs_str("c6wifi", "ssid"), "zq9net") && fs_str("c6wifi", "pass") && !strcmp(fs_str("c6wifi", "pass"), WF_PASS) && g_fs_commits == 1 && g_fs_sets == 2 && l1 == 1);
        c6_wifi_start();
        printf("wf1 (b) second start: migration lines in total=%d\n", log_count(m, "WIFI credentials migrated"));
        CHECK(log_count(m, "WIFI credentials migrated") == 1 && g_fs_commits == 1);
        reset_case(); c6_wifi_start(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        printf("wf1 (c) nothing: connects=%d state=%d ap up=%d ap sets=%d\n", g_w_connects, (int)c6_wifi_state(), c6_wifi_ap_up(), g_w_ap_sets);
        CHECK(g_w_connects == 0 && c6_wifi_state() == C6_WIFI_NO_CREDENTIALS && c6_wifi_ap_up() && g_w_ap_sets == 1 && g_w_sta_sets == 0 && g_fs_sets == 0);
        (void)ok;
    }
    verdict("wf1", "c6_wifi_start: stored pair used with nothing written; the build's pair migrated once with one commit and one log line; neither: no connect, NO_CREDENTIALS, the hotspot at once");

    /* wf2. validation */
    {   wf_boot_stored(); int sets = g_w_sta_sets, fsets = g_fs_sets, commits = g_fs_commits, fstarts = timer_by_name("wifi_fail")->starts, fstops = timer_by_name("wifi_fail")->stops;
        char longssid[40], longpass[80]; memset(longssid, 'a', 33); longssid[33] = 0; memset(longpass, 'p', 64); longpass[64] = 0;
        const char *bs[] = {"", longssid, "zq9\x07net", "zq9net", "zq9net", "zq9net", "zq9net"};
        const char *bp[] = {"pw-zq9ok", "pw-zq9ok", "pw-zq9ok", "x", "1234567", longpass, "pw-zq9-\n"};
        int all = 1; g_wseq[0] = 0;
        for (int i = 0; i < 7; i++) { int r = c6_wifi_set(bs[i], bp[i]); if (r != ESP_ERR_INVALID_ARG) { all = 0; printf("wf2 bad case %d returned %d\n", i, r); } }
        printf("wf2 bad inputs: all INVALID_ARG=%d, esp_wifi calls='%s', sta sets +%d, nvs sets +%d, commits +%d\n", all, g_wseq, g_w_sta_sets - sets, g_fs_sets - fsets, g_fs_commits - commits);
        CHECK(all && g_wseq[0] == 0 && g_w_sta_sets == sets && g_fs_sets == fsets && g_fs_commits == commits
              && timer_by_name("wifi_fail")->starts == fstarts && timer_by_name("wifi_fail")->stops == fstops);
        char s32[40], p63[70]; memset(s32, 's', 32); s32[32] = 0; memset(p63, 'p', 63); p63[63] = 0;
        const char *gs[] = {"a", s32, "zq9net", "zq9net", "zq9net"}; const char *gp[] = {"", "", "", "12345678", p63};
        int good = 0; for (int i = 0; i < 5; i++) good += c6_wifi_set(gs[i], gp[i]) == ESP_OK;
        printf("wf2 edges accepted: %d of 5 (ssid 1 and 32 chars, pass empty, 8 and 63 chars)\n", good);
        CHECK(good == 5);
    }
    verdict("wf2", "c6_wifi_set refuses ssid 0/33/control, pass 1/7/64/control as INVALID_ARG with NVS, driver and timers untouched; the edges 1, 32, empty, 8, 63 are accepted");

    /* wf3. set while connected and mid-backoff */
    for (int mid = 0; mid < 2; mid++) {
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        if (mid) { wf_disc(201); fake_advance(1000000); wf_disc(201); fake_advance(2000000); wf_disc(201); } else { wf_got_ip(); fake_advance(5000000); }
        int64_t backoff_before = s_backoff_ms; int c0 = g_fs_commits, s0 = g_fs_sets; g_wseq[0] = 0; int64_t t = fake_now;
        int r = c6_wifi_set("zq9net", WF_PASS);
        struct hq_timer *rt = timer_by_name("wifi_retry"), *ft = timer_by_name("wifi_fail");
        printf("wf3 %s: backoff %lld ms before -> %u after; calls='%s'; commits +%d, sets +%d; retry timer active=%d; fail timer due in %lld us; ip=%d state=%d ret=%d\n",
               mid ? "mid-backoff" : "connected", (long long)backoff_before, (unsigned)s_backoff_ms, g_wseq, g_fs_commits - c0, g_fs_sets - s0, rt->active, (long long)(ft->due - t), s_have_ip, (int)c6_wifi_state(), r);
        CHECK(r == ESP_OK && !strcmp(g_wseq, "disconnect set_config:STA connect") && g_w_state_refusals == 0 && g_fs_commits - c0 == 1 && g_fs_sets - s0 == 2 && s_backoff_ms == 1000 && !rt->active
              && ft->active && ft->due == t + 60000000 && (mid ? !s_have_ip : true) && c6_wifi_state() == C6_WIFI_CONNECTING);
        CHECK(mid ? backoff_before > 1000 : backoff_before == 1000);
    }
    {   /* the station still connecting (the hotspot is open): set_config is refused with ESP_ERR_WIFI_STATE unless the disconnect came first */
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL); fake_advance(61000000);
        struct hq_timer *ft = timer_by_name("wifi_fail"); int conn0 = g_w_connects, fst0 = ft->starts; g_wseq[0] = 0;
        int ap = c6_wifi_ap_up(), r = c6_wifi_set("zq9new", WF_PASS);
        printf("wf3 while connecting, hotspot up=%d: ret=%d, calls='%s', STATE refusals=%d, connect +%d, fail timer re-armed +%d, state=%d\n",
               ap, r, g_wseq, g_w_state_refusals, g_w_connects - conn0, ft->starts - fst0, (int)c6_wifi_state());
        CHECK(ap && r == ESP_OK && !strcmp(g_wseq, "disconnect set_config:STA connect") && g_w_state_refusals == 0 && g_w_connects == conn0 + 1
              && ft->starts == fst0 + 1 && c6_wifi_state() == C6_WIFI_CONNECTING && !strcmp((const char *)g_w_sta_ssid, "zq9new"));
    }
    verdict("wf3", "c6_wifi_set while connected, mid-backoff and while connecting: disconnect, set_config, connect in that order (set_config is not refused with ESP_ERR_WIFI_STATE); both keys, one commit; backoff 1 s; the 60 s timer re-armed to now+60 s; returns before any IP");

    /* wf4. the 60 s failure timer */
    {   wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        fake_advance(59900000);
        int st1 = (int)c6_wifi_state(), up1 = c6_wifi_ap_up(); fake_advance(100000);
        printf("wf4 no IP: at 59.9 s state=%d ap=%d; at 60.0 s state=%d ap=%d ap config sets=%d until-ip=%d\n", st1, up1, (int)c6_wifi_state(), c6_wifi_ap_up(), g_w_ap_sets, s_ap_until_ip);
        CHECK(st1 == C6_WIFI_CONNECTING && !up1 && c6_wifi_state() == C6_WIFI_FAILED && c6_wifi_ap_up() && g_w_ap_sets == 1 && s_ap_until_ip && wf_ap_cb_up == 1);
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        fake_advance(30000000); wf_got_ip(); int marks = g_nmarks; uint32_t m0 = g_nmarks ? g_marks[0] : 0; int active = timer_by_name("wifi_fail")->active; fake_advance(31000000);
        printf("wf4 IP at 30 s: at 61 s state=%d ap=%d fail timer active (after GOT_IP)=%d marks=%d first mark=0x%x\n", (int)c6_wifi_state(), c6_wifi_ap_up(), active, marks, m0);
        CHECK(c6_wifi_state() == C6_WIFI_CONNECTED && !c6_wifi_ap_up() && !active && marks == 1 && m0 == C6_ST_BOARD && g_w_ap_sets == 0 && g_nmarks == 1);
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        fake_advance(40000000); c6_wifi_set("zq9net", WF_PASS); fake_advance(59900000);
        int st3 = (int)c6_wifi_state(), up3 = c6_wifi_ap_up(); fake_advance(100000);
        printf("wf4 set at 40 s: at 99.9 s state=%d ap=%d; at 100.0 s state=%d ap=%d\n", st3, up3, (int)c6_wifi_state(), c6_wifi_ap_up());
        CHECK(st3 == C6_WIFI_CONNECTING && !up3 && c6_wifi_state() == C6_WIFI_FAILED && c6_wifi_ap_up());
    }
    verdict("wf4", "the 60 s failure timer: CONNECTING and no hotspot at 59.9 s, FAILED and the hotspot once at 60.0 s; an IP cancels it and marks BOARD once; a c6_wifi_set restarts the 60 s from then");

    /* wf5. hotspot lifetime */
    {   wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL); fake_advance(60000000);
        int up0 = c6_wifi_ap_up(); wf_got_ip(); fake_advance(9900000); int up1 = c6_wifi_ap_up(); fake_advance(100000);
        printf("wf5 opened by the timer: up=%d; IP; at +9.9 s up=%d; at +10.0 s up=%d mode=%d cb up/down=%d/%d\n", up0, up1, c6_wifi_ap_up(), g_w_mode, wf_ap_cb_up, wf_ap_cb_down);
        CHECK(up0 && up1 && !c6_wifi_ap_up() && g_w_mode == WIFI_MODE_STA && wf_ap_cb_up == 1 && wf_ap_cb_down == 1);
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL); wf_got_ip(); int mode0 = g_w_mode;
        c6_wifi_ap_start(600); int mode1 = g_w_mode; fake_advance(599000000); int up2 = c6_wifi_ap_up(); fake_advance(1000000);
        printf("wf5 ap_start(600) with an IP: mode %d -> %d; up at 599 s=%d; at 600 s up=%d mode=%d\n", mode0, mode1, up2, c6_wifi_ap_up(), g_w_mode);
        CHECK(mode0 == WIFI_MODE_STA && mode1 == WIFI_MODE_APSTA && up2 && !c6_wifi_ap_up() && g_w_mode == WIFI_MODE_STA);
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL); c6_wifi_ap_start(600); wf_got_ip(); fake_advance(11000000); int upi = c6_wifi_ap_up();
        fake_advance(588000000); int upj = c6_wifi_ap_up(); fake_advance(1000000);
        printf("wf5 ap_start(600) with no IP yet, then an IP: up at +11 s=%d (not IP + 10 s); at 599 s=%d; at 600 s up=%d mode=%d\n", upi, upj, c6_wifi_ap_up(), g_w_mode);
        CHECK(upi && upj && !c6_wifi_ap_up() && g_w_mode == WIFI_MODE_STA);
        int downs = wf_ap_cb_down, modes = g_w_set_modes; g_wseq[0] = 0; c6_wifi_ap_stop();
        printf("wf5 ap_stop while down: calls='%s' cb down +%d\n", g_wseq, wf_ap_cb_down - downs);
        CHECK(wf_ap_cb_down == downs && g_w_set_modes == modes && g_wseq[0] == 0);
    }
    verdict("wf5", "the hotspot: opened by the timer it closes 10.0 s after an IP; ap_start(600) holds to 600 s, an IP in the meantime included; ap_stop while down changes nothing; APSTA only while it is up");

    /* wf6. the hotspot's configuration over two cycles */
    {   char keep[16]; snprintf(keep, sizeof keep, "%s", s_host); snprintf(s_host, sizeof s_host, "c6-a1b2c3");
        wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        c6_wifi_ap_start(30); const wifi_config_t c1 = g_w_ap_cfg; int led1 = (s_active >> C6_LED_AP) & 1; c6_wifi_ap_stop(); int led1b = (s_active >> C6_LED_AP) & 1;
        g_w_sta_info_ok = 1; g_w_sta_channel = 6; wf_got_ip();
        c6_wifi_ap_start(30); const wifi_config_t c2 = g_w_ap_cfg; int led2 = (s_active >> C6_LED_AP) & 1; fake_advance(30000000); int led2b = (s_active >> C6_LED_AP) & 1;
        printf("wf6 cycle 1: ssid='%.*s' len=%d auth=%d max=%d ch=%d; cycle 2: ssid='%.*s' ch=%d; netif creates=%d; cb up/down=%d/%d; led %d/%d %d/%d\n",
               c1.ap.ssid_len, c1.ap.ssid, c1.ap.ssid_len, (int)c1.ap.authmode, c1.ap.max_connection, c1.ap.channel, c2.ap.ssid_len, c2.ap.ssid, c2.ap.channel,
               g_w_ap_netif_creates, wf_ap_cb_up, wf_ap_cb_down, led1, led1b, led2, led2b);
        CHECK(c1.ap.ssid_len == 21 && !memcmp(c1.ap.ssid, "Zigbee-Anthema-a1b2c3", 21) && c1.ap.authmode == WIFI_AUTH_OPEN && c1.ap.max_connection == 2 && c1.ap.channel == 1
              && c2.ap.ssid_len == 21 && !memcmp(c2.ap.ssid, "Zigbee-Anthema-a1b2c3", 21) && c2.ap.authmode == WIFI_AUTH_OPEN && c2.ap.max_connection == 2 && c2.ap.channel == 6);
        CHECK(g_w_ap_netif_creates == 1 && wf_ap_cb_up == 2 && wf_ap_cb_down == 2 && led1 == 1 && led1b == 0 && led2 == 1 && led2b == 0);
        snprintf(s_host, sizeof s_host, "%s", keep);
    }
    verdict("wf6", "the hotspot's configuration: Zigbee-Anthema-+six hex, open, 2 clients, the STA's channel else 1; the AP netif made once; on_ap and the AP LED set and cleared per cycle");

    /* wf7. the retry backoff */
    {   wf_boot_stored(); wf_ev(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        static const int64_t want[7] = {1000, 2000, 4000, 8000, 16000, 30000, 30000}; int ok = 1; size_t m = g_loglen;
        for (int i = 0; i < 7; i++) {
            wf_disc(201); int64_t d = timer_in_us("wifi_retry");
            if (d != want[i] * 1000) { ok = 0; printf("wf7 disconnect %d: retry in %lld us, wanted %lld\n", i, (long long)d, (long long)want[i] * 1000); }
            fake_advance(d > 0 ? d : 0);
        }
        wf_got_ip(); wf_disc(8); int64_t after = timer_in_us("wifi_retry");
        printf("wf7 delays as wanted=%d; after GOT_IP the next delay is %lld us; reconnects=%d; ctx violations=%d; log 'retry in 30000 ms' lines=%d\n", ok, (long long)after, g_w_connects, g_ctx_viol, log_count(m, "retry in 30000 ms"));
        CHECK(ok && after == 1000000 && g_w_connects == 8 && g_ctx_viol == 0 && log_count(m, "retry in 30000 ms") == 2);
    }
    verdict("wf7", "seven disconnects retry after 1, 2, 4, 8, 16, 30, 30 s; after GOT_IP the next is 1 s; the handler only starts timers (no NVS, no c6_wifi_set)");

    /* wf8. scan */
    {   wf_boot_stored();
        int n = 0;
        for (int i = 0; i < 17; i++) { snprintf((char *)g_w_aps[n].ssid, 33, "zq9-net%02d", i); g_w_aps[n].rssi = (int8_t)(-30 - 3 * i); g_w_aps[n].authmode = i % 2 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN; n++; }
        snprintf((char *)g_w_aps[n].ssid, 33, "zq9-net02"); g_w_aps[n].rssi = -90; n++;           /* weaker duplicates */
        snprintf((char *)g_w_aps[n].ssid, 33, "zq9-net07"); g_w_aps[n].rssi = -95; n++;
        g_w_aps[n].ssid[0] = 0; g_w_aps[n].rssi = -10; n++;                                         /* hidden and the strongest of all */
        g_w_nap = n;
        nres = 0; nested_rc = 0; nested_calls = 0;
        int rc = c6_wifi_scan(scan_cb, NULL);
        int desc = 1, uniq = 1, hidden = 0; for (int i = 0; i < nres; i++) { if (i && res[i].rssi > res[i - 1].rssi) desc = 0; if (!res[i].ssid[0]) hidden = 1; for (int j = 0; j < i; j++) if (!strcmp(res[i].ssid, res[j].ssid)) uniq = 0; }
        int kept = 0; for (int i = 0; i < nres; i++) if (!strcmp(res[i].ssid, "zq9-net02")) kept = res[i].rssi;
        printf("wf8 20 APs: %d results, strongest first=%d, unique=%d, hidden present=%d, zq9-net02 kept at %d dBm, nested scan returned 0x%x (INVALID_STATE=0x%x), first scan rc=%d, scans started=%d\n", nres, desc, uniq, hidden, kept, nested_rc, ESP_ERR_INVALID_STATE, rc, g_w_scan_calls);
        CHECK(n == 20 && rc == ESP_OK && nres == 16 && desc && uniq && !hidden && kept == -36 && nested_rc == ESP_ERR_INVALID_STATE && g_w_scan_calls == 1);
        nres = 0; nested_calls = 1; int rc2 = c6_wifi_scan(scan_cb, NULL); g_w_nap = 0; int before = nres; nres = 0; int rc3 = c6_wifi_scan(scan_cb, NULL);
        printf("wf8 a scan afterwards works (%d, %d results); zero networks: rc=%d, results=%d\n", rc2, before, rc3, nres);
        CHECK(rc2 == ESP_OK && before == 16 && rc3 == ESP_OK && nres == 0 && !s_scanning);
    }
    verdict("wf8", "c6_wifi_scan: at most 16 results, strongest first, one per SSID with the strongest kept, hidden dropped; a nested call is INVALID_STATE and the first completes");
}
