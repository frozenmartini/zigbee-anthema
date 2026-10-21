/* bt1-bt5: c6_button.c. btn_task is stepped on fake time (vTaskDelay budget) with a scripted pin; the callback is the real button_action behind a recorder. */
static int64_t bt_t0; static int bt_lows[8][2], bt_nlows;             /* [from_ms, to_ms) the pin is low, relative to the task's start; to < 0: forever */
static int bt_g[16], bt_ng, bt_g_pin[16], bt_bad_ctx; static unsigned bt_g_act[16]; static int64_t bt_g_at[16];
static int bt_sample(int64_t now_us) { int64_t ms = (now_us - bt_t0) / 1000; for (int i = 0; i < bt_nlows; i++) if (ms >= bt_lows[i][0] && (bt_lows[i][1] < 0 || ms < bt_lows[i][1])) return 0; return 1; }
static void bt_cb(int g) {
    if (bt_ng < 16) { bt_g[bt_ng] = g; bt_g_at[bt_ng] = (fake_now - bt_t0) / 1000; bt_g_pin[bt_ng] = bt_sample(fake_now); bt_ng++; }
    if (g_crit != 0 || g_in_stack_ctx) bt_bad_ctx++;
    button_action(g);
    if (bt_ng >= 1 && bt_ng <= 16) bt_g_act[bt_ng - 1] = s_active;         /* the LED sources right after the action, still inside the poll that raised the gesture */
}
static void bt_setup(void) { reset_case(); bt_ng = 0; bt_bad_ctx = 0; bt_nlows = 0; g_btn_sample = bt_sample; g_auto_bearer = 0; wf_store("zq9net", WF_PASS); c6_wifi_start(); au_state(true, false); c6_button_start(bt_cb); }
static void bt_low(int from, int to) { bt_lows[bt_nlows][0] = from; bt_lows[bt_nlows][1] = to; bt_nlows++; }
static void bt_run(int total_ms) { bt_t0 = fake_now; g_delay_budget = total_ms / 20; if (!setjmp(loop_done)) btn_task(NULL); g_delay_budget = -1; }
static int bt_wopens(size_t m) { return log_count(m, "AUTH claim window open for 600 s"); }

static unsigned bt5_act[1200]; static int bt5_ms[1200], bt5_n;
static int bt5_sample(int64_t now_us) { if (bt5_n < 1200) { bt5_act[bt5_n] = s_active; bt5_ms[bt5_n] = (int)((now_us - bt_t0) / 1000); bt5_n++; } return bt_sample(now_us); }
static int bt5_first(int src) { for (int i = 0; i < bt5_n; i++) if (bt5_act[i] & (1u << src)) return bt5_ms[i]; return -1; }
static int bt5_last(int src) { int t = -1; for (int i = 0; i < bt5_n; i++) if (bt5_act[i] & (1u << src)) t = bt5_ms[i]; return t; }
static int bt5_rises(int src) { int n = 0; for (int i = 0; i < bt5_n; i++) n += !!(bt5_act[i] & (1u << src)) && (i == 0 || !(bt5_act[i - 1] & (1u << src))); return n; }

static void probe_bt(void) {
    puts("==== bt: the button ====");
    /* bt1. presses under 1 s, holds of 1 s to under 3 s */
    {   const int press[3] = {100, 500, 980}, hold[3] = {1000, 2000, 2980};
        for (int k = 0; k < 3; k++) {
            bt_setup(); size_t m = g_loglen; bt_low(100, 100 + press[k]); bt_run(100 + press[k] + 200);
            printf("bt1 press %d ms: gestures=%d first=%d at %lld ms (release at %d ms); window_open_for(600) calls=%d; ap sets=%d; cleared lines=%d\n", press[k], bt_ng, bt_ng ? bt_g[0] : -1, bt_ng ? (long long)bt_g_at[0] : -1LL, 100 + press[k], bt_wopens(m), g_w_ap_sets, log_count(m, "AUTH claim cleared"));
            CHECK(bt_ng == 1 && bt_g[0] == C6_BUTTON_PRESS && bt_g_at[0] >= 100 + press[k] && bt_g_at[0] <= 100 + press[k] + 60 && bt_wopens(m) == 1 && g_w_ap_sets == 0 && g_closes == 0);
        }
        for (int k = 0; k < 3; k++) {
            bt_setup(); size_t m = g_loglen; bt_low(100, 100 + hold[k]); bt_run(100 + hold[k] + 200);
            printf("bt1 hold %d ms: gestures=%d; window calls=%d; ap sets=%d; closes=%d\n", hold[k], bt_ng, bt_wopens(m), g_w_ap_sets, g_closes);
            CHECK(bt_ng == 0 && bt_wopens(m) == 0 && g_w_ap_sets == 0 && g_closes == 0 && c6_auth_claimed());
        }
    }
    verdict("bt1", "a press under 1 s gives one PRESS at the release and one window_open_for(600); holds of 1 s to under 3 s give no gesture and no action");

    /* bt2. holds of 3 s to under 10 s */
    {   const int hold[3] = {3000, 6000, 9980};
        for (int k = 0; k < 3; k++) {
            bt_setup(); size_t m = g_loglen; bt_low(100, 100 + hold[k]); bt_run(100 + hold[k] + 200);
            printf("bt2 hold %d ms: gestures=%d first=%d at %lld ms; ap up=%d ap config sets=%d ap timer %lld s; window calls=%d; claimed=%d closes=%d\n", hold[k], bt_ng, bt_ng ? bt_g[0] : -1, bt_ng ? (long long)bt_g_at[0] : -1LL, c6_wifi_ap_up(), g_w_ap_sets, (long long)(timer_in_us("wifi_ap") / 1000000), bt_wopens(m), c6_auth_claimed(), g_closes);
            CHECK(bt_ng == 1 && bt_g[0] == C6_BUTTON_HOLD_SHORT && bt_g_at[0] >= 100 + hold[k] && bt_g_at[0] <= 100 + hold[k] + 60 && c6_wifi_ap_up() && g_w_ap_sets == 1
                  && timer_in_us("wifi_ap") > 590000000 && timer_in_us("wifi_ap") <= 600000000 && bt_wopens(m) == 0 && c6_auth_claimed() && g_closes == 0);
        }
    }
    verdict("bt2", "a hold of 3 s to under 10 s gives one HOLD_SHORT at the release and c6_wifi_ap_start(600) once; no auth action");

    /* bt3. the 10 s hold acts at 10 s, with the button still down */
    {   const int rel[3] = {10000, 15000, 30000};
        for (int k = 0; k < 3; k++) {
            bt_setup(); size_t m = g_loglen; bt_low(100, 100 + rel[k]); bt_run(100 + rel[k] + 300);
            printf("bt3 released at %d ms: gestures=%d first=%d at %lld ms with the pin %s; cleared lines=%d closes=%d claimed=%d window calls=%d ap sets=%d\n", rel[k], bt_ng, bt_ng ? bt_g[0] : -1, bt_ng ? (long long)bt_g_at[0] : -1LL,
                   bt_ng && bt_g_pin[0] == 0 ? "low" : "high (the first sample of the release)", log_count(m, "AUTH claim cleared"), g_closes, c6_auth_claimed(), log_count(m, "AUTH claim window open for 600 s"), g_w_ap_sets);
            CHECK(bt_ng == 1 && bt_g[0] == C6_BUTTON_HOLD_LONG && bt_g_at[0] >= 10100 && bt_g_at[0] <= 10140 && (k == 0 || bt_g_pin[0] == 0) && log_count(m, "AUTH claim cleared") == 1 && g_closes == 1 && !c6_auth_claimed() && g_w_ap_sets == 0);
        }
    }
    verdict("bt3", "a hold acts once at 10 s as HOLD_LONG with the pin still low (c6_auth_clear once); its release gives nothing, no HOLD_SHORT, no PRESS");

    /* bt4. glitches, sequences, power-up, context and configuration */
    {   bt_setup(); bt_low(100, 120); bt_run(1000);
        printf("bt4 one low sample in a high period: gestures=%d\n", bt_ng);
        CHECK(bt_ng == 0);
        bt_setup(); bt_low(100, 1000); bt_low(1020, 3100); bt_run(3500);                      /* one high sample at 1000-1019 inside a hold of 3 s */
        printf("bt4 one high sample inside a hold: gestures=%d first=%d\n", bt_ng, bt_ng ? bt_g[0] : -1);
        CHECK(bt_ng == 1 && bt_g[0] == C6_BUTTON_HOLD_SHORT);
        bt_setup(); bt_low(100, 400); bt_low(1000, 4100); bt_low(5000, 5300); bt_run(6000);
        printf("bt4 press, hold-short, press: %d gestures: %d %d %d; callback outside any section and any stack context: %s\n", bt_ng, bt_g[0], bt_g[1], bt_g[2], bt_bad_ctx ? "NO" : "yes");
        CHECK(bt_ng == 3 && bt_g[0] == C6_BUTTON_PRESS && bt_g[1] == C6_BUTTON_HOLD_SHORT && bt_g[2] == C6_BUTTON_PRESS && !bt_bad_ctx);
        bt_setup(); bt_low(-5000, 2000); bt_low(3000, 3300); bt_run(4000);
        printf("bt4 held at power-up (released at 2.0 s), then a press: gestures=%d first=%d at %lld ms\n", bt_ng, bt_ng ? bt_g[0] : -1, bt_ng ? (long long)bt_g_at[0] : -1LL);
        CHECK(bt_ng == 1 && bt_g[0] == C6_BUTTON_PRESS && bt_g_at[0] >= 3300);
        bt_setup(); bt_low(-5000, 12000); bt_run(14000);
        printf("bt4 held at power-up through 12 s: gestures=%d (nothing, not even HOLD_LONG)\n", bt_ng);
        CHECK(bt_ng == 0 && c6_auth_claimed());
        bt_setup();
        printf("bt4 pin configuration: mask 0x%llx mode=%d pull-up=%d pull-down=%d intr=%d; task 'button' made %d time(s), stack %u\n", (unsigned long long)g_gpio_cfg.pin_bit_mask, g_gpio_cfg.mode, g_gpio_cfg.pull_up_en, g_gpio_cfg.pull_down_en, g_gpio_cfg.intr_type, tasks_named("button"), task_stack("button"));
        CHECK(g_gpio_cfg.pin_bit_mask == (1ULL << CONFIG_C6_BUTTON_GPIO) && g_gpio_cfg.mode == GPIO_MODE_INPUT && g_gpio_cfg.pull_up_en == GPIO_PULLUP_ENABLE && g_gpio_cfg.pull_down_en == GPIO_PULLDOWN_DISABLE
              && g_gpio_cfg.intr_type == GPIO_INTR_DISABLE && tasks_named("button") == 1 && task_stack("button") == 6144);
    }
    verdict("bt4", "debounce and start-up: a single glitch changes nothing; press, hold-short, press gives three gestures in order; a button down at power-up gives nothing until released; the callback runs in the task, outside any section; the pin is input, pull-up, active low");

    /* bt5. the colour LED during a hold: HOLD_GREEN from 3 s, HOLD_RED from 10 s; the sources are sampled on every pin read (every 20 ms, before that tick's step) */
    {   /* (a) a hold of 5 s */
        bt_setup(); size_t m = g_loglen; g_btn_sample = bt5_sample; bt5_n = 0; bt_low(100, 5100); bt_run(5400);
        int g_first = bt5_first(C6_LED_HOLD_GREEN), g_rises = bt5_rises(C6_LED_HOLD_GREEN), g_last = !!(bt5_act[bt5_n - 1] & (1u << C6_LED_HOLD_GREEN)), r_any = bt5_rises(C6_LED_HOLD_RED);
        int at_cb_green = !!(bt_g_act[0] & (1u << C6_LED_HOLD_GREEN)), at_cb_ap = !!(bt_g_act[0] & (1u << C6_LED_AP));
        printf("bt5 a hold of 5,000 ms: HOLD_GREEN first seen at %d ms, set %d time(s), still set at the end=%d, HOLD_RED sets=%d; 'BUTTON held 3 s' lines=%d; gestures=%d first=%d; at the HOLD_SHORT action green still set=%d and AP set=%d\n",
               g_first, g_rises, g_last, r_any, log_count(m, "BUTTON held 3 s"), bt_ng, bt_ng ? bt_g[0] : -1, at_cb_green, at_cb_ap);
        CHECK(g_first >= 3100 && g_first <= 3140 && g_rises == 1 && !g_last && r_any == 0 && log_count(m, "BUTTON held 3 s") == 1 && bt_ng == 1 && bt_g[0] == C6_BUTTON_HOLD_SHORT && at_cb_green && at_cb_ap && (bt5_act[bt5_n - 1] & (1u << C6_LED_AP)) && c6_wifi_ap_up());
        /* (b) holds that end before 3 s */
        const int shorts[2] = {980, 2980};
        for (int k = 0; k < 2; k++) {
            bt_setup(); m = g_loglen; g_btn_sample = bt5_sample; bt5_n = 0; bt_low(100, 100 + shorts[k]); bt_run(100 + shorts[k] + 300);
            int sg = bt5_rises(C6_LED_HOLD_GREEN), sr = bt5_rises(C6_LED_HOLD_RED);
            printf("bt5 a hold of %d ms: HOLD_GREEN sets=%d, HOLD_RED sets=%d, 'BUTTON held 3 s' lines=%d, gestures=%d\n", shorts[k], sg, sr, log_count(m, "BUTTON held 3 s"), bt_ng);
            CHECK(sg == 0 && sr == 0 && log_count(m, "BUTTON held 3 s") == 0 && bt_ng == (k == 0 ? 1 : 0));          /* 980 ms is a PRESS, 2,980 ms gives no gesture (as bt1) */
        }
        /* (c) a hold released at 12 s */
        bt_setup(); m = g_loglen; g_btn_sample = bt5_sample; bt5_n = 0; bt_low(100, 12100); bt_run(12400);
        int c_gfirst = bt5_first(C6_LED_HOLD_GREEN), c_grises = bt5_rises(C6_LED_HOLD_GREEN), c_rfirst = bt5_first(C6_LED_HOLD_RED), c_rrises = bt5_rises(C6_LED_HOLD_RED);
        int c_gend = bt5_last(C6_LED_HOLD_GREEN), c_rend = bt5_last(C6_LED_HOLD_RED);
        unsigned cb = bt_g_act[0];
        int c_un_before = !!(bt5_act[0] & (1u << C6_LED_UNCLAIMED)), c_un_after = !!(bt5_act[bt5_n - 1] & (1u << C6_LED_UNCLAIMED));
        printf("bt5 a hold released at 12,000 ms: HOLD_GREEN first %d ms, last seen %d ms (sets %d); HOLD_RED first %d ms, last seen %d ms (sets %d); at the HOLD_LONG action green=%d red=%d UNCLAIMED=%d claimed=%d; UNCLAIMED before/after=%d/%d; gestures=%d first=%d\n",
               c_gfirst, c_gend, c_grises, c_rfirst, c_rend, c_rrises, !!(cb & (1u << C6_LED_HOLD_GREEN)), !!(cb & (1u << C6_LED_HOLD_RED)), !!(cb & (1u << C6_LED_UNCLAIMED)), c6_auth_claimed(), c_un_before, c_un_after, bt_ng, bt_ng ? bt_g[0] : -1);
        CHECK(c_gfirst >= 3100 && c_gfirst <= 3140 && c_grises == 1 && c_rrises == 1 && c_rfirst >= 10100 && c_rfirst <= 10180 && c_gend < c_rfirst && c_gend >= 10100 - 40
              && (cb & (1u << C6_LED_HOLD_GREEN)) && !(cb & (1u << C6_LED_HOLD_RED)) && (cb & (1u << C6_LED_UNCLAIMED)) && !c_un_before && c_un_after
              && !(bt5_act[bt5_n - 1] & (1u << C6_LED_HOLD_RED)) && c_rend >= 12100 - 40 && c_rend <= 12100 + 40 && bt_ng == 1 && bt_g[0] == C6_BUTTON_HOLD_LONG);
        /* (d) held from power-up through 12 s: nothing shown */
        bt_setup(); g_btn_sample = bt5_sample; bt5_n = 0; bt_low(-5000, 12000); bt_run(14000);
        printf("bt5 held at power-up through 12 s: HOLD_GREEN sets=%d, HOLD_RED sets=%d, gestures=%d\n", bt5_rises(C6_LED_HOLD_GREEN), bt5_rises(C6_LED_HOLD_RED), bt_ng);
        CHECK(bt5_rises(C6_LED_HOLD_GREEN) == 0 && bt5_rises(C6_LED_HOLD_RED) == 0 && bt_ng == 0);
    }
    verdict("bt5", "the colour LED during a hold: HOLD_GREEN comes on at 3 s (one 'BUTTON held 3 s' line) and is still set when the HOLD_SHORT action runs (AP is up by then), off at the release; HOLD_GREEN gives way to HOLD_RED at the 10 s step with the claim already cleared, and HOLD_RED goes at the release, UNCLAIMED showing after; holds under 3 s and a button down at power-up show nothing");
}
