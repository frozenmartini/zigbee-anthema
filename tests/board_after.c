/* ===== BOARD STATE RESET (after the extracted firmware: the statics of the new files are in scope here) ===== */
/* the strong hooks of events.c are extracted under real_* names; these record each call and pass it on */
static int g_mark_pct[512]; static char g_mark_st[512][12];      /* 0.6.0: the update object's state and percent as each C6_ST_UPDATE mark was made (up10) */
void c6_app_state_mark(uint32_t keys)
{
    if (g_nmarks < 512) {
        g_marks[g_nmarks] = keys;
        if (keys & C6_ST_UPDATE) {
            g_mark_pct[g_nmarks] = s_update.percent;
            snprintf(g_mark_st[g_nmarks], sizeof g_mark_st[0], "%s", s_update.state);
        }
    }
    g_nmarks++;
    real_c6_app_state_mark(keys);
}
void c6_app_events_close(void)
{
    g_closes++;
    real_c6_app_events_close();
    if (s_ev_lock && *(int *)s_ev_lock) g_sem_viol++;               /* the close leaves the events lock free */
}
static void board_reset(void)
{
    g_nmarks = g_closes = 0;
    g_cj_fail_at = 0;
    timers_forget(); g_in_stack_ctx = 0; g_ctx_viol_total += g_ctx_viol; g_ctx_viol = 0;
    g_sem_calls = 0; g_ntasks = 0; g_restarts = 0; g_flash_erases = 0; g_nnotify = 0; g_notify_in_crit = 0;
    fs_clear(); g_rand_calls = 0; g_rand_seq_n = 0; g_sha_calls = 0; g_sha_use_fixed = false; g_sha_len = 0;
    chunks_clear(); g_mdns_adds = g_mdns_txt_sets = g_mdns_name_sets = g_mdns_add_err = g_mdns_other_services = 0; g_mdns_ntxt = 0;
    g_btn_sample = NULL;
    g_factory_new = true; g_joined = false; g_commission_ret = 0; g_commissions = 0; g_commission_afters = 0; g_local_resets = 0;
    g_order = g_order_pan = g_order_start = g_set_use_pan_calls = 0; g_zb_rx_last = 0;
    g_got_ip_calls = 0;
    s_claimed = false; memset(s_digest, 0, sizeof s_digest); s_window_until_us = 0; s_window_timer = NULL; s_lock = NULL;
    s_active = 0; led_s_started = false; memset(s_since_us, 0, sizeof s_since_us); s_strip = NULL; s_shown = 0; s_identify_rgb = C6_LED_WHITE;
    g_ncolor = g_color_nct = 0; g_hu_wrong_cl = 0; g_chunk_want = NULL; g_chunk_wrong = 0; g_hu_creates = g_hu_registers = g_hu_endpoints = 0; g_hu_ep_id = g_hu_manuf_code = g_hu_app_device_id = -1; g_hu_handler_set = 0; memset(g_hu_manuf, 0, sizeof g_hu_manuf); memset(g_hu_model, 0, sizeof g_hu_model); memset(&g_hu_cfg, 0, sizeof g_hu_cfg);
    g_rgb = g_rgb_pending = 0; g_rgb_writes = 0; g_strip_new_ret = 0; g_strip_news = 0; for (int i = 0; i < 40; i++) g_pin_lvl[i] = -1;
    disc_s_started = false; s_identify_timer = NULL;
    s_dirty = 0; s_seq = 0; s_events_task = NULL; s_ev_lock = NULL; s_stream = NULL; s_sent_us = 0; s_ev_id = 0; s_refresh_us = 0;
    s_group = 0; s_group_dirty = false; s_zb_erasing = false; s_reporting_ok = false; s_zb_started = false; s_zb_rejoining = false; s_zb_searching = false; s_steer_timer = NULL; s_steer_fast_us = 0;
    s_hue_boot = true;                                              /* settings.c: the boot value, as cfg_load leaves it on an empty store */
    s_light_by = NULL; s_light_at_us = 0; s_hub_seq = 0; s_presses = 0; s_release_last = NULL; s_button_at_us = 0;
    s_ring_total = 0;
    bt_s_started = false; bt_s_cb = NULL;
    g_fs_commit_hook = NULL;
    g_cv_nvs_open_hook = NULL; g_cv_fail_key = NULL; g_cv_erase_error = 0;
    wstub_reset(); g_kconfig_wifi_ssid[0] = g_kconfig_wifi_pass[0] = 0;
    s_netif = s_ap_netif = NULL; s_reconnect_timer = s_fail_timer = s_ap_timer = NULL; s_backoff_ms = WIFI_BACKOFF_MIN_MS; wf_s_state = C6_WIFI_NO_CREDENTIALS;
    s_have_creds = s_have_ip = s_ap_up = s_ap_until_ip = s_scanning = false; wf_s_ssid[0] = 0; s_ap_cb = NULL;
    lw_reset(); s_dns_task = NULL; s_dns_want = false; s_dns_sock = -1;
    s_ota_busy = false; s_ev = NULL; s_zb_ready_logged = false;       /* 0.6.0: c6_base.c's flag, event group and once-flag; the update object back to a fresh boot's */
    s_update = (c6_update_t){.state = "idle", .version = "", .percent = -1, .error = ""};
    ota_stub_reset(); eg_stub_reset(); g_recv_hook = NULL; g_recv_hook_at = 0;
    usb_reset(); memset(&s_improv, 0, sizeof s_improv); s_improv_session = false; s_improv_sent = -1; s_improv_provisioning = false; s_improv_deadline_us = 0;
}

/* a claimed board whose token is G_TOKEN, and requests that carry it unless a probe says otherwise (g_auto_bearer) */
static void claim_now(void)
{
    unsigned char d[32];
    mbedtls_sha256((const unsigned char *)G_TOKEN, strlen(G_TOKEN), d, 0);
    s_claimed = true; memcpy(s_digest, d, 32); g_auto_bearer = 1; g_sha_calls = 0;
}

/* an unclaimed boot of c6_auth (au, ld) */
static void au_boot(void) { s_claimed = false; memset(s_digest, 0, sizeof s_digest); g_auto_bearer = 0; c6_auth_start(); }

/* ---- request helpers that need the firmware's handlers ---- */
static httpd_req_t bd_req;
static int bd_put(const char *body) { req_init(&bd_req); req_body(&bd_req, body); api_call(settings_put, &bd_req); return bd_req.code; }
static int bd_get(void) { req_init(&bd_req); api_call(settings_get, &bd_req); return bd_req.code; }
