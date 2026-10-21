/* vf1-vf8: c6_base.c's first-start check. verify_task is the real function, run to completion on fake time: its waits are the event-group stub's WaitBits (which moves
 * fake_now forward, firing the bits a probe scheduled with eg_at) and vTaskDelay. The Kconfig numbers (120 / 600 / 60 s) are read from c6_base/Kconfig by build_harness.py.
 * weak_verify_task is the same function compiled against the weak c6_app_verify_zigbee (false: c6_base, c6_coord); brk_c6_base_zigbee_ready is c6_base_zigbee_ready compiled
 * with CONFIG_C6_VERIFY_BREAK_TEST 1. */
static int64_t vf_t0;
static void vf_boot(void) { reset_case(); s_ev = xEventGroupCreate(); vf_t0 = fake_now; }
static void vf_at(int64_t sec, EventBits_t bits) { eg_at(vf_t0 + sec * 1000000, bits); }
static void vf_run(int weak) { vf_t0 = fake_now; if (weak) weak_verify_task(NULL); else verify_task(NULL); }
static int64_t vf_s(int64_t at_us) { return (at_us - vf_t0) / 1000000; }
#define VF_NET (BIT0 | BIT1)                        /* BIT_GOT_IP | BIT_HTTPD */

static void vf_zr_up(void) { zr_boot(); s_ev = xEventGroupCreate(); }

static void probe_vf(void) {
    puts("==== vf: the first-start check (verify_task) ====");
    /* vf1. all three stages */
    {   vf_boot(); vf_at(5, VF_NET); vf_at(40, BIT2); size_t m = g_loglen; vf_run(0);
        printf("vf1 IP+HTTP at 5 s, zigbee at 40 s: valid x%d at %lld s, rolled back x%d; lines: net=%d zigbee=%d valid=%d, no-zigbee-check=%d\n", g_marked_valid, (long long)vf_s(g_valid_at), g_rolled_back,
               log_count(m, "VERIFY: wifi+http up after 5 s"), log_count(m, "VERIFY: zigbee ready after 40 s"), log_count(m, "VERIFY: new image marked valid"), log_count(m, "no zigbee check"));
        CHECK(g_marked_valid == 1 && vf_s(g_valid_at) == 100 && g_rolled_back == 0);
        CHECK(log_count(m, "VERIFY: wifi+http up after 5 s") == 1 && log_count(m, "VERIFY: zigbee ready after 40 s") == 1 && log_count(m, "VERIFY: new image marked valid") == 1 && log_count(m, "no zigbee check") == 0);
    }
    verdict("vf1", "an image that asks for Zigbee: Wi-Fi + HTTP at 5 s, Zigbee at 40 s: marked valid once at 100 s (40 + the 60 s stable delay), never rolled back, the three lines");

    /* vf2. Zigbee never comes */
    {   vf_boot(); vf_at(5, VF_NET); size_t m = g_loglen; vf_run(0);
        printf("vf2 IP+HTTP at 5 s, zigbee never: rolled back x%d at %lld s, valid x%d; lines: not back=%d, marked valid=%d\n", g_rolled_back, (long long)vf_s(g_rolled_back_at), g_marked_valid, log_count(m, "VERIFY: zigbee not back after 600 s"), log_count(m, "marked valid"));
        CHECK(g_rolled_back == 1 && vf_s(g_rolled_back_at) == 600 && g_marked_valid == 0 && log_count(m, "VERIFY: zigbee not back after 600 s") == 1 && log_count(m, "marked valid") == 0);
    }
    verdict("vf2", "Wi-Fi + HTTP at 5 s but Zigbee never: rolled back once at 600 s from the task's start, never marked valid, the line says so");

    /* vf3. the network never comes */
    {   vf_boot(); size_t m = g_loglen; vf_run(0);
        printf("vf3 no IP or HTTP: rolled back x%d at %lld s, valid x%d; 'not reachable' lines=%d, any zigbee line=%d\n", g_rolled_back, (long long)vf_s(g_rolled_back_at), g_marked_valid, log_count(m, "VERIFY: not reachable after 120 s"), log_count(m, "VERIFY: zigbee"));
        CHECK(g_rolled_back == 1 && vf_s(g_rolled_back_at) == 120 && g_marked_valid == 0 && log_count(m, "VERIFY: not reachable after 120 s") == 1 && log_count(m, "VERIFY: zigbee") == 0 && log_count(m, "no zigbee check") == 0);
    }
    verdict("vf3", "no Wi-Fi + HTTP: rolled back at 120 s as in 0.5.2, with no Zigbee stage at all");

    /* vf4. an image that does not ask */
    {   vf_boot(); vf_at(5, VF_NET); size_t m = g_loglen; vf_run(1);
        printf("vf4 the weak hook (false): valid x%d at %lld s, rolled back x%d; 'no zigbee check' lines=%d\n", g_marked_valid, (long long)vf_s(g_valid_at), g_rolled_back, log_count(m, "VERIFY: no zigbee check in this image"));
        CHECK(g_marked_valid == 1 && vf_s(g_valid_at) == 65 && g_rolled_back == 0 && log_count(m, "VERIFY: no zigbee check in this image") == 1 && log_count(m, "zigbee ready after") == 0);
        CHECK(!weak_c6_app_verify_zigbee() && c6_app_verify_zigbee());                  /* the two hooks: c6_base's default is false, the bulb's is true */
    }
    verdict("vf4", "an image whose hook is the weak default (false): no Zigbee wait, valid at 65 s (5 + 60), the \"no zigbee check in this image\" line; the bulb's hook is true");

    /* vf5. Zigbee before the network */
    {   vf_boot(); vf_at(2, BIT2); vf_at(100, VF_NET); size_t m = g_loglen; vf_run(0);
        printf("vf5 zigbee at 2 s, IP+HTTP at 100 s: valid x%d at %lld s, rolled back x%d; lines: net=%d zigbee=%d\n", g_marked_valid, (long long)vf_s(g_valid_at), g_rolled_back, log_count(m, "VERIFY: wifi+http up after 100 s"), log_count(m, "VERIFY: zigbee ready after 100 s"));
        CHECK(g_marked_valid == 1 && vf_s(g_valid_at) == 160 && g_rolled_back == 0 && log_count(m, "VERIFY: wifi+http up after 100 s") == 1 && log_count(m, "VERIFY: zigbee ready after 100 s") == 1);
    }
    verdict("vf5", "Zigbee ready at 2 s, before the network (100 s): the early bit counts (the first wait does not clear it), valid at 160 s");

    /* vf6. c6_base_zigbee_ready */
    {   reset_case(); size_t m = g_loglen; c6_base_zigbee_ready();                       /* before the event group exists: safe, silent, nothing set */
        printf("vf6 before s_ev exists: bit word=0x%x, lines=%d\n", g_eg_word, log_count(m, "VERIFY: zigbee"));
        CHECK(g_eg_word == 0 && log_count(m, "VERIFY: zigbee") == 0 && !s_zb_ready_logged);
        vf_boot(); m = g_loglen; c6_base_zigbee_ready(); c6_base_zigbee_ready(); c6_base_zigbee_ready();
        printf("vf6 three calls: bit word=0x%x, 'VERIFY: zigbee ready' lines=%d\n", g_eg_word, log_count(m, "VERIFY: zigbee ready"));
        CHECK((g_eg_word & BIT2) && log_count(m, "VERIFY: zigbee ready") == 1 && log_count(m, "ignored") == 0);
        vf_boot(); m = g_loglen; brk_c6_base_zigbee_ready(); brk_c6_base_zigbee_ready(); brk_c6_base_zigbee_ready();
        printf("vf6 break-test build, three calls: bit word=0x%x, 'ignored' lines=%d, 'VERIFY: zigbee ready' lines=%d\n", g_eg_word, log_count(m, "VERIFY: zigbee ready ignored"), log_count(m, "VERIFY: zigbee ready\n"));
        CHECK(!(g_eg_word & BIT2) && log_count(m, "VERIFY: zigbee ready ignored (C6_VERIFY_BREAK_TEST)") == 1);
        vf_at(5, VF_NET); size_t m2 = g_loglen; vf_run(0);                                  /* the break build: the bit never comes, so the check ends as vf2 */
        printf("vf6 break-test build then the check: rolled back x%d at %lld s, valid x%d\n", g_rolled_back, (long long)vf_s(g_rolled_back_at), g_marked_valid);
        CHECK(g_rolled_back == 1 && vf_s(g_rolled_back_at) == 600 && g_marked_valid == 0 && log_count(m2, "VERIFY: zigbee not back after 600 s") == 1);
    }
    verdict("vf6", "c6_base_zigbee_ready: silent before s_ev exists; three calls set the bit and log once; compiled with the break-test macro it sets nothing, says so once, and the check then rolls back at 600 s");

    /* vf7. the window runs from the start of the task */
    {   vf_boot(); vf_at(110, VF_NET); vf_at(590, BIT2); vf_run(0);
        printf("vf7 IP at 110 s, zigbee at 590 s: valid x%d at %lld s, rolled back x%d\n", g_marked_valid, (long long)vf_s(g_valid_at), g_rolled_back);
        CHECK(g_marked_valid == 1 && vf_s(g_valid_at) == 650 && g_rolled_back == 0);
        vf_boot(); vf_at(110, VF_NET); vf_at(610, BIT2); vf_run(0);
        printf("vf7 IP at 110 s, zigbee at 610 s: rolled back x%d at %lld s (not 710), valid x%d\n", g_rolled_back, (long long)vf_s(g_rolled_back_at), g_marked_valid);
        CHECK(g_rolled_back == 1 && vf_s(g_rolled_back_at) == 600 && g_marked_valid == 0);
    }
    verdict("vf7", "the Zigbee window is 600 s from the task's start, not from the network: IP at 110 s and Zigbee at 590 s is valid; Zigbee at 610 s is rolled back at 600 s, not 710");

    /* vf8. the three on_signal branches, and what must not count */
    {   const struct {const char *what; int fn; int type; int st;} ok[3] = {
            {"factory new (FIRST_START, status 0)", 0, EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0}, {"rejoined (REBOOT, status 0, network kept)", 1, EZB_BDB_SIGNAL_DEVICE_REBOOT, 0}, {"steering success", 2, EZB_BDB_SIGNAL_STEERING, 0}};
        for (int k = 0; k < 3; k++) {
            vf_zr_up(); g_factory_new = k == 0; g_joined = k != 0; size_t m = g_loglen; zr_sig(ok[k].type, ok[k].st);
            printf("vf8 %s: bit word=0x%x, 'VERIFY: zigbee ready' lines=%d\n", ok[k].what, g_eg_word, log_count(m, "VERIFY: zigbee ready"));
            CHECK((g_eg_word & BIT2) && log_count(m, "VERIFY: zigbee ready") == 1);
        }
        const struct {const char *what; int leave; int type; int st;} no[6] = {
            {"a failed REBOOT (status 3)", 0, EZB_BDB_SIGNAL_DEVICE_REBOOT, 3}, {"a failed FIRST_START (status 3)", 0, EZB_BDB_SIGNAL_DEVICE_FIRST_START, 3}, {"a failed steering (status 3)", 0, EZB_BDB_SIGNAL_STEERING, 3},
            {"a leave (reset type)", 1, EZB_ZDO_LEAVE_TYPE_RESET, 0}, {"a leave (rejoin type)", 1, 1, 0}, {"SKIP_STARTUP", 0, EZB_ZDO_SIGNAL_SKIP_STARTUP, 0}};
        for (int k = 0; k < 6; k++) {
            vf_zr_up(); g_factory_new = false; g_joined = true; size_t m = g_loglen;
            if (no[k].leave) zr_leave(no[k].type); else zr_sig(no[k].type, no[k].st);
            printf("vf8 %s alone: bit word=0x%x, 'VERIFY: zigbee' lines=%d\n", no[k].what, g_eg_word, log_count(m, "VERIFY: zigbee"));
            CHECK(!(g_eg_word & BIT2) && log_count(m, "VERIFY: zigbee") == 0);
        }
    }
    verdict("vf8", "on_signal sets the Zigbee bit in the factory-new, rejoined and steering-success branches, and in none of: a failed REBOOT / FIRST_START / steering, a leave of either type, SKIP_STARTUP");
}
