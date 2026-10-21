/* ld1-ld4: c6_led.c driven a step at a time (led_step) on fake time; the colour LED is read as what the led_strip stub last refreshed (g_rgb) and how often (g_rgb_writes).
 * The expected colours and rhythms are written out here, not read from the firmware: ld_sc() is the level scaling, ld_col() the colour of each source, ld_exp() the pattern. */
static uint32_t ld_rgb[600]; static int ld_ms[600];
static int64_t ld_t0;                                               /* fake time of the set the pattern is phased from */
static uint32_t ld_sc(uint32_t rgb) { uint32_t r = ((rgb >> 16) & 0xFF) * CONFIG_C6_RGB_LEVEL / 255, g = ((rgb >> 8) & 0xFF) * CONFIG_C6_RGB_LEVEL / 255, b = (rgb & 0xFF) * CONFIG_C6_RGB_LEVEL / 255; return r << 16 | g << 8 | b; }
static uint32_t ld_col(int src) { return src == C6_LED_IDENTIFY ? 0xFFFFFFu : (src == C6_LED_HOLD_RED || src == C6_LED_UNCLAIMED) ? 0xFF0000u : 0x00FF00u; }
/* what a source alone shows t ms after its set: its colour scaled, or 0 */
static uint32_t ld_exp(int src, int t, uint32_t colour) {
    int on = src == C6_LED_IDENTIFY ? 1 : (src == C6_LED_HOLD_RED || src == C6_LED_HOLD_GREEN) ? (t % 125) < 63 : ((t % 2000) < 100 || ((t % 2000) >= 200 && (t % 2000) < 300));
    return on ? ld_sc(colour) : 0;
}
static void ld_run(int n) { for (int i = 0; i < n && i < 600; i++) { led_step(fake_now); ld_rgb[i] = g_rgb; ld_ms[i] = (int)((fake_now - ld_t0) / 1000); fake_now += 10000; } }
static int ld_changes(uint32_t before, int n) { int c = 0; uint32_t last = before; for (int i = 0; i < n; i++) { c += ld_rgb[i] != last; last = ld_rgb[i]; } return c; }
static int ld_matches(int src, int n, uint32_t colour) { int bad = 0; for (int i = 0; i < n; i++) bad += ld_rgb[i] != ld_exp(src, ld_ms[i], colour); return bad; }
static void ld_fresh(void) { reset_case(); c6_led_start(); ld_t0 = fake_now; }

static httpd_req_t ld_r;
static int ld_ident(int api, const char *query, const char *s) { req_init(&ld_r); ld_r.query = query; ld_r.s = s; if (api) api_call(api_identify_post, &ld_r); else identify_post(&ld_r); return ld_r.code; }
static void ld_claimed_boot(void) { uint8_t d[32]; mbedtls_sha256((const unsigned char *)G_TOKEN, strlen(G_TOKEN), d, 0); fs_put("c6claim", "sha", 4, 0, d, 32); s_claimed = false; memset(s_digest, 0, sizeof s_digest); g_auto_bearer = 0; c6_auth_start(); }

static void probe_ld(void) {
    puts("==== ld: the colour LED ====");
    /* ld1. the mux over every set order of the five sources, then every clear order */
    {   int perm[120][5], np = 0, bad = 0, steps = 0;
        for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) for (int c = 0; c < 5; c++) for (int d = 0; d < 5; d++) for (int e = 0; e < 5; e++)
            if (a != b && a != c && a != d && a != e && b != c && b != d && b != e && c != d && c != e && d != e) { perm[np][0] = a; perm[np][1] = b; perm[np][2] = c; perm[np][3] = d; perm[np][4] = e; np++; }
        for (int p = 0; p < np; p++) {
            for (int rev = 0; rev < 2; rev++) {                    /* clear in the order set, then in the reverse order */
                ld_fresh(); unsigned model = 0;
                for (int i = 0; i < 5; i++) {
                    c6_led_set((c6_led_source_t)perm[p][i], true); model |= 1u << perm[p][i]; steps++;
                    int want = -1; for (int s = 0; s < 5; s++) if (model & (1u << s)) { want = s; break; }
                    led_step(fake_now);
                    if (led_winner(s_active) != want || s_active != model || g_rgb != ld_sc(ld_col(want))) bad++;
                }
                for (int k = 0; k < 5; k++) {
                    int src = perm[p][rev ? 4 - k : k];
                    c6_led_set((c6_led_source_t)src, false); model &= ~(1u << src); steps++;
                    int want = -1; for (int s = 0; s < 5; s++) if (model & (1u << s)) { want = s; break; }
                    led_step(fake_now);
                    if (led_winner(s_active) != want || s_active != model || g_rgb != (want < 0 ? 0u : ld_sc(ld_col(want)))) bad++;
                }
            }
        }
        printf("ld1 %d orders x 2 clear orders, %d set/clear steps, mux or colour mismatches=%d\n", np, steps, bad);
        CHECK(np == 120 && bad == 0);
        ld_fresh(); c6_led_set(C6_LED_AP, true); c6_led_set(C6_LED_AP, true); unsigned once = s_active; int64_t since = s_since_us[C6_LED_AP];
        fake_now += 500000; c6_led_set(C6_LED_AP, true); c6_led_set(C6_LED_UNCLAIMED, false); c6_led_set((c6_led_source_t)7, true); c6_led_set((c6_led_source_t)-1, true); c6_led_set(C6_LED_SOURCES, true);
        printf("ld1 set twice / clear when not set / out of range: active=0x%x (was 0x%x), phase kept=%d\n", s_active, once, s_since_us[C6_LED_AP] == since);
        CHECK(s_active == once && once == 1u << C6_LED_AP && s_since_us[C6_LED_AP] == since);
        ld_fresh(); c6_led_set(C6_LED_UNCLAIMED, true); ld_run(30); int w0 = g_rgb_writes; c6_led_set(C6_LED_IDENTIFY, true); ld_run(100);
        int white = 0; for (int i = 0; i < 100; i++) white += ld_rgb[i] == ld_sc(0xFFFFFF);
        printf("ld1 IDENTIFY over UNCLAIMED: %d of 100 samples white, %d strip writes for the change\n", white, g_rgb_writes - w0);
        CHECK(white == 100 && g_rgb_writes - w0 == 1);
        c6_led_set(C6_LED_IDENTIFY, false); ld_run(300);
        int back = ld_matches(C6_LED_UNCLAIMED, 300, 0xFF0000);
        printf("ld1 identify cleared: the next 3 s show UNCLAIMED's own phase again, mismatches=%d\n", back);
        CHECK(back == 0 && !(s_active & (1u << C6_LED_IDENTIFY)));
    }
    verdict("ld1", "the lowest enum value wins (IDENTIFY, HOLD_RED, HOLD_GREEN, AP, UNCLAIMED) over every set order and both clear orders, and the colour follows the winner; none: off; a repeated set, a clear when not set and an out-of-range source change nothing");

    /* ld2. the five patterns alone, the start-up, and a strip that cannot be set up */
    {   const int srcs[5] = {C6_LED_IDENTIFY, C6_LED_HOLD_RED, C6_LED_HOLD_GREEN, C6_LED_AP, C6_LED_UNCLAIMED};
        const char *names[5] = {"IDENTIFY", "HOLD_RED", "HOLD_GREEN", "AP", "UNCLAIMED"};
        for (int k = 0; k < 5; k++) {
            ld_fresh(); int w0 = g_rgb_writes; c6_led_set((c6_led_source_t)srcs[k], true); ld_run(200);
            int mism = ld_matches(srcs[k], 200, ld_col(srcs[k])), ch = ld_changes(0, 200), lit = 0; for (int i = 0; i < 200; i++) lit += ld_rgb[i] != 0;
            printf("ld2 %s alone over 2 s: mismatches=%d, lit samples=%d of 200, colour changes=%d, strip writes=%d\n", names[k], mism, lit, ch, g_rgb_writes - w0);
            CHECK(mism == 0 && lit > 0 && g_rgb_writes - w0 == ch);
        }
        ld_fresh(); c6_led_identify_rgb(0x00FF80); c6_led_set(C6_LED_IDENTIFY, true); ld_run(50);
        int cm = ld_matches(C6_LED_IDENTIFY, 50, 0x00FF80);
        printf("ld2 IDENTIFY in the colour 00FF80: mismatches=%d, shown 0x%06X\n", cm, ld_rgb[0]);
        CHECK(cm == 0 && ld_rgb[0] == ld_sc(0x00FF80) && ld_rgb[0] != ld_sc(0xFFFFFF));
        ld_fresh();
        printf("ld2 after c6_led_start: strip made %d time(s) on GPIO %d, %d LED, WS2812=%d GRB=%d res %u Hz; dark %06X after %d write(s); blue LED GPIO %d level %d; power GPIO %d level %d; task 'led' x%d\n",
               g_strip_news, g_strip_gpio, g_strip_leds, g_strip_model == LED_MODEL_WS2812, g_strip_fmt == LED_STRIP_COLOR_COMPONENT_FMT_GRB, g_strip_res, g_rgb, g_rgb_writes, CONFIG_C6_LED_GPIO, g_pin_lvl[CONFIG_C6_LED_GPIO], CONFIG_C6_RGB_PWR_GPIO, g_pin_lvl[CONFIG_C6_RGB_PWR_GPIO], tasks_named("led"));
        CHECK(g_strip_news == 1 && g_strip_gpio == CONFIG_C6_RGB_GPIO && g_strip_leds == 1 && g_strip_model == LED_MODEL_WS2812 && g_strip_fmt == LED_STRIP_COLOR_COMPONENT_FMT_GRB && g_strip_res == 10000000u
              && g_rgb == 0 && g_rgb_writes == 1 && g_pin_lvl[CONFIG_C6_LED_GPIO] == 0 && g_pin_lvl[CONFIG_C6_RGB_PWR_GPIO] == 1 && tasks_named("led") == 1);
        c6_led_start(); CHECK(g_strip_news == 1 && tasks_named("led") == 1);               /* a second start does nothing */
        reset_case(); g_strip_new_ret = 1; size_t m = g_loglen; c6_led_start(); c6_led_set(C6_LED_UNCLAIMED, true); ld_t0 = fake_now; ld_run(30);
        printf("ld2 the strip cannot be set up: log lines=%d, strip writes=%d, task made=%d\n", log_count(m, "was not set up"), g_rgb_writes, tasks_named("led"));
        CHECK(log_count(m, "was not set up") == 1 && g_rgb_writes == 0 && tasks_named("led") == 1);
    }
    verdict("ld2", "each source alone over 2 s: IDENTIFY solid (white, or the colour given); HOLD_RED and HOLD_GREEN 8 Hz (lit the first 63 ms of every 125); AP green and UNCLAIMED red, lit at 0-99 and 200-299 ms of every 2,000; the strip is written only when the colour shown changes; start-up: dark once, blue LED low, power pin high; a strip that cannot be set up stays dark");

    /* ld3. POST /identify and /api/v1/identify with and without rgb */
    {   for (int api = 0; api < 2; api++) {
            const char *path = api ? "/api/v1/identify" : "/identify";
            ld_fresh(); int code = ld_ident(api, "s=5&rgb=FF00FF", api ? "5" : NULL); led_step(fake_now);
            int64_t due = timer_in_us("identify"); uint32_t shown = g_rgb; fake_advance(due); led_step(fake_now);
            printf("ld3 %s s=5&rgb=FF00FF: http=%d, shown 0x%06X (want 0x%06X), timer %lld s, after it 0x%06X\n", path, code, shown, ld_sc(0xFF00FF), (long long)(due / 1000000), g_rgb);
            CHECK(code == 200 && shown == ld_sc(0xFF00FF) && due == 5000000 && g_rgb == 0 && !(s_active & (1u << C6_LED_IDENTIFY)));
            ld_fresh(); code = ld_ident(api, "s=5&rgb=ff00ff", api ? "5" : NULL); led_step(fake_now);
            printf("ld3 %s rgb=ff00ff (lower case): http=%d, shown 0x%06X\n", path, code, g_rgb);
            CHECK(code == 200 && g_rgb == ld_sc(0xFF00FF));
            ld_fresh(); code = ld_ident(api, api ? NULL : "s=5", api ? "5" : NULL); led_step(fake_now);
            printf("ld3 %s no rgb: http=%d, shown 0x%06X (white scaled 0x%06X)\n", path, code, g_rgb, ld_sc(0xFFFFFF));
            CHECK(code == 200 && g_rgb == ld_sc(0xFFFFFF));
            ld_fresh(); code = ld_ident(api, "s=5&rgb=FF00FF", api ? "5" : NULL); led_step(fake_now); code = ld_ident(api, "s=5", api ? "5" : NULL); led_step(fake_now);
            printf("ld3 %s a second call without rgb after magenta: shown 0x%06X (the colour is taken afresh)\n", path, g_rgb);
            CHECK(code == 200 && g_rgb == ld_sc(0xFFFFFF));
            const char *bad[] = {"rgb=FF00F", "rgb=GG00FF", "rgb=FF00FF00", "rgb=", "rgb=#FF00FF"};
            for (int k = 0; k < 5; k++) {
                ld_fresh(); uint32_t before = g_rgb; int w0 = g_rgb_writes; code = ld_ident(api, bad[k], api ? "5" : NULL); led_step(fake_now);
                int isbad = strstr(ld_r.response, "bad_request") != NULL;
                printf("ld3 %s %s: http=%d bad_request=%d, IDENTIFY set=%d, shown 0x%06X (was 0x%06X), strip writes %d, timer %lld\n", path, bad[k], code, isbad, !!(s_active & (1u << C6_LED_IDENTIFY)), g_rgb, before, g_rgb_writes - w0, (long long)timer_in_us("identify"));
                CHECK(code == 400 && isbad && !(s_active & (1u << C6_LED_IDENTIFY)) && g_rgb == before && g_rgb_writes == w0 && timer_in_us("identify") < 0);
            }
            ld_fresh(); ld_ident(api, "s=5&rgb=FF00FF", api ? "5" : NULL); led_step(fake_now); int64_t left = timer_in_us("identify"); code = ld_ident(api, "s=9&rgb=12345", api ? "9" : NULL); led_step(fake_now);
            printf("ld3 %s a bad rgb while one runs: http=%d, still 0x%06X, timer %lld us (was %lld)\n", path, code, g_rgb, (long long)timer_in_us("identify"), (long long)left);
            CHECK(code == 400 && g_rgb == ld_sc(0xFF00FF) && timer_in_us("identify") == left);
        }
    }
    verdict("ld3", "identify takes rgb=RRGGBB in either case on /identify and /api/v1/identify (shown scaled, off after the seconds), white without it; five malformed values are 400 bad_request and change nothing, a running identify included");

    /* ld4. the claim: UNCLAIMED follows the claim, not the window */
    {   uint8_t rs[32]; for (int i = 0; i < 32; i++) rs[i] = (uint8_t)(i * 9 + 3);
        ld_fresh(); memcpy(g_rand_seq, rs, 32); g_rand_seq_n = 32; au_boot(); ld_t0 = fake_now; led_step(fake_now);
        int boot_on = !!(s_active & (1u << C6_LED_UNCLAIMED)); uint32_t boot_rgb = g_rgb;
        httpd_req_t r; req_init(&r); api_call(api_claim_post, &r); led_step(fake_now);
        int claim_off = !(s_active & (1u << C6_LED_UNCLAIMED)); uint32_t claim_rgb = g_rgb;
        printf("ld4 boot unclaimed: UNCLAIMED=%d shown 0x%06X; after POST /claim (http %d): UNCLAIMED=%d shown 0x%06X\n", boot_on, boot_rgb, r.code, !claim_off, claim_rgb);
        CHECK(boot_on && boot_rgb == ld_sc(0xFF0000) && r.code == 200 && claim_off && claim_rgb == 0);
        fake_advance(1000000); c6_auth_clear(); ld_t0 = fake_now; led_step(fake_now);
        int clr_on = !!(s_active & (1u << C6_LED_UNCLAIMED)); uint32_t clr_rgb = g_rgb;
        fake_advance(600000000); int win = c6_auth_window_open(); led_step(fake_now);
        int late_on = !!(s_active & (1u << C6_LED_UNCLAIMED)); uint32_t late_rgb = g_rgb;
        printf("ld4 after c6_auth_clear: UNCLAIMED=%d shown 0x%06X; 600 s later the window is open=%d (closed line %d), UNCLAIMED=%d shown 0x%06X\n", clr_on, clr_rgb, win, log_count(0, "AUTH claim window closed"), late_on, late_rgb);
        CHECK(clr_on && clr_rgb == ld_sc(0xFF0000) && !win && log_count(0, "AUTH claim window closed") >= 1 && late_on && late_rgb == ld_sc(0xFF0000));
        ld_fresh(); ld_claimed_boot(); g_auto_bearer = 1; led_step(fake_now);
        int cb_off = !(s_active & (1u << C6_LED_UNCLAIMED)); uint32_t cb_rgb = g_rgb; int cb_win = c6_auth_window_open();
        fake_advance(100000000); httpd_req_t r2; req_init(&r2); api_call(api_release_post, &r2); led_step(fake_now);
        int rel_on = !!(s_active & (1u << C6_LED_UNCLAIMED)); uint32_t rel_rgb = g_rgb;
        printf("ld4 boot claimed: UNCLAIMED=%d shown 0x%06X (window open=%d); POST /api/v1/release (http %d): UNCLAIMED=%d shown 0x%06X\n", !cb_off, cb_rgb, cb_win, r2.code, rel_on, rel_rgb);
        CHECK(cb_off && cb_rgb == 0 && cb_win && r2.code == 200 && rel_on && rel_rgb == ld_sc(0xFF0000));
        ld_fresh(); ld_claimed_boot(); ld_run(300);
        int dark = 0; for (int i = 0; i < 300; i++) dark += ld_rgb[i] == 0;
        printf("ld4 a claimed boot over 3 s: dark samples %d of 300\n", dark);
        CHECK(dark == 300);
    }
    verdict("ld4", "the UNCLAIMED source follows the claim: on at an unclaimed boot, off at the claim, on after c6_auth_clear and after POST /api/v1/release, off at a claimed boot; the window closing does not turn it off");
}
