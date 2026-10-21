/* up1-up13: POST /api/v1/update (c6_base.c's update_post, the real function), against the OTA stubs of board_stubs_ota.c and the real SHA-256 of board_stubs.c.
 * A probe makes a body of N bytes, the X-Firmware-SHA256 header from the same stub, calls the handler and reads: the status line and JSON, the stub counters,
 * the update object (c6_base_update_info), the C6_ST_UPDATE marks (with the object's state and percent as each was made: g_mark_st / g_mark_pct), g_closes, the reboot task. */
static unsigned char up_body[1500000];          /* 1,466,176 B is the real image's size; any pattern will do */
static httpd_req_t up_r;
static char up_hex[65];
static c6_update_t up_u;
static void up_boot(void) {                     /* a claimed board (the token is the request's bearer), a slot whose description is {c6_bulb, 0.6.0}, nothing running */
    static int filled; if (!filled) { for (size_t i = 0; i < sizeof up_body; i++) up_body[i] = (unsigned char)(i * 7u + (i >> 8) + 3u); filled = 1; }
    reset_case();
}
static const char *up_hdr_of(size_t n) { sha256_hex_of(up_body, n, up_hex); return up_hex; }
static void up_info(void);
static int up_send(size_t body_n, size_t content_len, const char *hdr) {        /* body_n bytes arrive, the request says content_len */
    req_init(&up_r); up_r.body = (const char *)up_body; up_r.body_len = body_n; up_r.content_len = content_len; up_r.h_sha = hdr; api_call(update_post, &up_r); up_info(); return up_r.code;
}
static int up_good(size_t n) { char h[65]; snprintf(h, sizeof h, "%s", up_hdr_of(n)); return up_send(n, n, h); }
static int up_err_is(int code, const char *word) {                               /* the answer is this status with {"error":"word",...} as JSON */
    char p[64]; snprintf(p, sizeof p, "\"error\":\"%s\"", word);
    return up_r.code == code && strstr(up_r.response, p) && !strcmp(up_r.type, "application/json");
}
static int up_says(const char *s) { return strstr(up_r.response, s) != NULL; }
static void up_info(void) { memset(&up_u, 0xAA, sizeof up_u); c6_base_update_info(&up_u); }
static int up_is(const char *state, int percent, const char *version, const char *error) { up_info(); return !strcmp(up_u.state, state) && up_u.percent == percent && !strcmp(up_u.version, version) && !strcmp(up_u.error, error); }
static int up_marks_from(int n0) { int c = 0; for (int i = n0; i < g_nmarks && i < 512; i++) c += (g_marks[i] & C6_ST_UPDATE) != 0; return c; }
static int up_untouched(void) { return g_ota_begins == 0 && g_ota_writes == 0 && g_ota_ends == 0 && g_ota_aborts == 0 && g_ota_boot_calls == 0 && g_ota_boot_set == NULL; }
/* the percent values the cadence rule gives for a body of n bytes in 4,096 B chunks (the cadence rule, written again here, not read from the firmware):
 * a mark when the integer percent is 5 or more above the last mark, or reaches 100 */
static int up_expect_pcts(size_t n, int *out) {
    int k = 0, last = 0; size_t w = 0;
    while (w < n) { size_t c = n - w < 4096 ? n - w : 4096; w += c; int p = (int)((unsigned long long)w * 100 / n); if (p >= 100 ? last < 100 : p - last >= 5) { out[k++] = p; last = p; } }
    return k;
}
static int up_pcts(int n0, int *out) {                                           /* the percents of the "receiving" marks made after the entry mark, from index n0 */
    int k = 0, seen_entry = 0;
    for (int i = n0; i < g_nmarks && i < 512; i++) if ((g_marks[i] & C6_ST_UPDATE) && !strcmp(g_mark_st[i], "receiving")) { if (!seen_entry) { seen_entry = 1; continue; } out[k++] = g_mark_pct[i]; }
    return k;
}
static httpd_req_t up_hq;
static int up_hook_ota_code, up_hook_upd_code, up_hook_ran; static char up_hook_ota_text[200], up_hook_upd_text[300];
static void up_hook_other_requests(httpd_req_t *r) {                              /* a second request arriving while the first one's body is being received */
    (void)r; static const char tiny[] = "0123456789"; int save = g_api_call; g_api_call = 0;          /* /ota's plain-text 409 is not an /api/v1 answer: keep it out of cx2's capture */
    up_hook_ran++;
    req_init(&up_hq); up_hq.body = tiny; up_hq.body_len = up_hq.content_len = sizeof tiny - 1; ota_post(&up_hq);
    up_hook_ota_code = up_hq.code; snprintf(up_hook_ota_text, sizeof up_hook_ota_text, "%.199s", up_hq.response);
    g_api_call = save;
    req_init(&up_hq); up_hq.body = tiny; up_hq.body_len = up_hq.content_len = sizeof tiny - 1; up_hq.h_sha = up_hdr_of(sizeof tiny - 1); update_post(&up_hq);
    up_hook_upd_code = up_hq.code; snprintf(up_hook_upd_text, sizeof up_hook_upd_text, "%.299s", up_hq.response);
}

static void probe_up(void) {
    puts("==== up: POST /api/v1/update ====");
    /* up1. a good image, and the SHA-256 stub against the FIPS vector */
    {   char abc[65]; sha256_hex_of("abc", 3, abc);
        printf("up1 SHA-256 of \"abc\": %s\n", abc);
        CHECK(!strcmp(abc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        up_boot(); size_t m = g_loglen; int m0 = g_nmarks; int c = up_good(12000);
        int pstep = 0, ordered = 1, seen_v = 0, seen_r = 0, last_pct = -1;
        for (int i = m0; i < g_nmarks && i < 512; i++) if (g_marks[i] & C6_ST_UPDATE) {
            if (!strcmp(g_mark_st[i], "receiving") && g_mark_pct[i] > 0 && g_mark_pct[i] < 100) pstep++;
            if (!strcmp(g_mark_st[i], "verifying")) seen_v = 1;
            if (!strcmp(g_mark_st[i], "rebooting")) seen_r = 1;
            if (g_mark_pct[i] < last_pct) ordered = 0;
            last_pct = g_mark_pct[i];
        }
        int rt = -1; for (int i = 0; i < g_ntasks; i++) if (!strcmp(g_tasks[i].name, "reboot")) rt = i;
        char dg[65]; for (int i = 0; i < 32; i++) snprintf(dg + 2 * i, 3, "%02x", g_ota_digest[i]);
        printf("up1 12,000 B with the right digest: http=%d %s; boot slot set=%d; closes=%d; reboot task=%d (made after %d closes); marks=%d (steps strictly between 0 and 100: %d, verifying=%d, rebooting=%d); written %zu B in %d writes, digest of the written bytes %s\n",
               c, up_r.response, g_ota_boot_set == &g_ota_next, g_closes, tasks_named("reboot"), rt >= 0 ? g_tasks[rt].closes_at : -1, up_marks_from(m0), pstep, seen_v, seen_r, g_ota_bytes, g_ota_writes, strcmp(dg, up_hdr_of(12000)) ? "DIFFERS" : "equals the header");
        CHECK(c == 200 && !strcmp(up_r.response, "{\"ok\":true,\"version\":\"0.6.0\"}") && !strcmp(up_r.type, "application/json"));
        CHECK(g_ota_boot_set == &g_ota_next && g_closes == 1 && tasks_named("reboot") == 1 && rt >= 0 && g_tasks[rt].closes_at == 1 && g_tasks[rt].stack == 6144);
        CHECK(up_marks_from(m0) >= 4 && pstep >= 1 && seen_v && seen_r && ordered && !strcmp(g_mark_st[g_nmarks - 1], "rebooting"));
        CHECK(g_ota_begins == 1 && g_ota_begin_len == 12000 && g_ota_bytes == 12000 && g_ota_ends == 1 && g_ota_aborts == 0 && g_ota_open == 0 && !strcmp(dg, up_hdr_of(12000)));
        CHECK(up_is("rebooting", 100, "0.6.0", ""));
        CHECK(log_count(m, "UPDATE: receiving 12000 bytes into ota_1") == 1 && log_count(m, "UPDATE: 12000 bytes to ota_1 in") == 1 && log_count(m, "rebooting") >= 1);
    }
    verdict("up1", "a good image (12,000 B, its SHA-256 in the header): 200 {ok, version}, the boot slot set, the stream closed once, the reboot task made after it, marks receiving / a percent step / verifying / rebooting, the object rebooting 0.6.0, the written bytes hash to the header");

    /* up2. the header: absent, 63 characters, a non-hex character */
    {   char h63[65], hg[65], hup[65]; const char *hs[4];
        up_boot(); snprintf(h63, sizeof h63, "%s", up_hdr_of(2000)); h63[63] = 0;
        snprintf(hg, sizeof hg, "%s", up_hdr_of(2000)); hg[10] = 'g';
        snprintf(hup, sizeof hup, "%s", up_hdr_of(2000)); for (char *p = hup; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 'a' + 'A');
        hs[0] = NULL; hs[1] = h63; hs[2] = hg; hs[3] = hup;
        for (int k = 0; k < 4; k++) {
            up_boot(); int m0 = g_nmarks; int c = up_send(2000, 2000, hs[k]);
            if (k < 3) {
                printf("up2 header %s: http=%d %s; begun=%d; object %s/%d/%s; marks=%d\n", k == 0 ? "absent" : k == 1 ? "63 chars" : "with a g", c, up_r.response, g_ota_begins, up_u.state, up_u.percent, up_u.error, up_marks_from(m0));
                CHECK(up_err_is(400, "bad_request") && up_says("X-Firmware-SHA256") && up_untouched() && up_is("idle", -1, "", "") && up_marks_from(m0) == 0 && !s_ota_busy);
            } else {
                printf("up2 header in upper-case hex: http=%d (the digest is the same number; 200 expected)\n", c);
                CHECK(c == 200 && up_is("rebooting", 100, "0.6.0", ""));
            }
        }
    }
    verdict("up2", "X-Firmware-SHA256 absent, 63 characters, or with a non-hex character: 400 bad_request naming the header, nothing begun, the object and the busy flag as they were; upper-case hex is accepted");

    /* up3. Content-Length 0 */
    {   up_boot(); int m0 = g_nmarks; int c = up_send(0, 0, up_hdr_of(10));
        printf("up3 content_len 0: http=%d %s; begun=%d; marks=%d\n", c, up_r.response, g_ota_begins, up_marks_from(m0));
        CHECK(up_err_is(400, "bad_request") && up_says("Content-Length") && up_untouched() && up_is("idle", -1, "", "") && up_marks_from(m0) == 0 && !s_ota_busy);
    }
    verdict("up3", "Content-Length 0: 400 bad_request naming Content-Length, nothing begun, the object unchanged");

    /* up4. larger than the slot; no slot */
    {   for (int k = 0; k < 2; k++) {
            up_boot(); if (k) g_ota_no_slot = 1; int m0 = g_nmarks;
            int c = up_send(4096, k ? 5000 : (size_t)g_ota_next.size + 1, up_hdr_of(4096));
            int pe = k ? up_says("no update slot") : up_says("larger than the update slot");
            printf("up4 %s: http=%d %s; begun=%d; object %s/%d/%s; busy=%d; marks=%d\n", k ? "no slot" : "one byte over the slot", c, up_r.response, g_ota_begins, up_u.state, up_u.percent, up_u.error, s_ota_busy, up_marks_from(m0));
            CHECK(up_err_is(413, "too_large") && pe && up_untouched() && up_is("idle", -1, "", "too_large") && !s_ota_busy && up_marks_from(m0) >= 1);
        }
        up_boot(); up_send(2000, (size_t)g_ota_next.size, up_hdr_of(2000));        /* exactly the slot's size is not too large: it is begun (then the body ends early) */
        printf("up4 exactly the slot's size: http=%d begun=%d\n", up_r.code, g_ota_begins);
        CHECK(g_ota_begins == 1 && !up_err_is(413, "too_large"));
    }
    verdict("up4", "an image over the slot, or no slot: 413 too_large, nothing begun, the object back at idle with error too_large and the busy flag clear; exactly the slot's size is begun");

    /* up5. digest mismatch */
    {   up_boot(); int m0 = g_nmarks; size_t m = g_loglen; int c = up_send(12000, 12000, up_hdr_of(11999));
        printf("up5 header is the SHA-256 of other bytes: http=%d %s; ends=%d boot set=%d aborts=%d; object %s/%s; busy=%d; marks=%d\n", c, up_r.response, g_ota_ends, g_ota_boot_set != NULL, g_ota_aborts, up_u.state, up_u.error, s_ota_busy, up_marks_from(m0));
        CHECK(up_err_is(400, "sha_mismatch") && g_ota_ends == 1 && g_ota_boot_set == NULL && g_ota_boot_calls == 0 && g_ota_aborts == 0 && up_is("idle", -1, "", "sha_mismatch") && !s_ota_busy);
        CHECK(g_closes == 0 && tasks_named("reboot") == 0 && log_count(m, "UPDATE: refused, sha_mismatch") == 1);
    }
    verdict("up5", "a digest that does not match the header: 400 sha_mismatch after esp_ota_end, the boot slot untouched, no reboot, the object idle with error sha_mismatch, the busy flag clear");

    /* up6. esp_ota_end rejects the image */
    {   up_boot(); g_ota_end_err = 5; size_t m = g_loglen; int c = up_good(12000);
        printf("up6 esp_ota_end rejects: http=%d %s; ends=%d aborts=%d boot set=%d; object %s/%s; busy=%d\n", c, up_r.response, g_ota_ends, g_ota_aborts, g_ota_boot_set != NULL, up_u.state, up_u.error, s_ota_busy);
        CHECK(up_err_is(400, "bad_image") && g_ota_ends == 1 && g_ota_aborts == 0 && g_ota_boot_set == NULL && up_is("idle", -1, "", "bad_image") && !s_ota_busy && log_count(m, "UPDATE: esp_ota_end") == 1);
    }
    verdict("up6", "esp_ota_end rejecting the image: 400 bad_image, no abort after end, the boot slot untouched, the object idle with error bad_image, the busy flag clear");

    /* up7. the slot holds another project's image */
    {   up_boot(); ota_set_slot_desc("c6_base", "0.6.0"); int c = up_good(12000);
        printf("up7 slot says c6_base: http=%d %s; boot set=%d; object %s/%s\n", c, up_r.response, g_ota_boot_set != NULL, up_u.state, up_u.error);
        CHECK(up_err_is(409, "wrong_firmware") && up_says("c6_base") && up_says("c6_bulb") && g_ota_boot_set == NULL && g_ota_boot_calls == 0 && up_is("idle", -1, "", "wrong_firmware") && !s_ota_busy);
        up_boot(); g_ota_desc_err = 1; c = up_good(12000);
        printf("up7 the slot's description unreadable: http=%d %s\n", c, up_r.response);
        CHECK(up_err_is(409, "wrong_firmware") && up_says("unreadable") && g_ota_boot_set == NULL && up_is("idle", -1, "", "wrong_firmware"));
    }
    verdict("up7", "the slot's image named for another project (or unreadable): 409 wrong_firmware naming both projects, the boot slot untouched, the object idle with error wrong_firmware");

    /* up8. the body ends early */
    {   up_boot(); int m0 = g_nmarks; int c = up_send(40000, 100000, up_hdr_of(100000));
        int last_rec = -1, any_verify = 0; for (int i = m0; i < g_nmarks && i < 512; i++) if (g_marks[i] & C6_ST_UPDATE) { if (!strcmp(g_mark_st[i], "receiving")) last_rec = g_mark_pct[i]; if (!strcmp(g_mark_st[i], "verifying")) any_verify = 1; }
        printf("up8 100,000 B announced, 40,000 B sent: http=%d %s; aborts=%d ends=%d; object %s/%s; busy=%d; last receiving mark %d %%; verifying mark=%d\n", c, up_r.response, g_ota_aborts, g_ota_ends, up_u.state, up_u.error, s_ota_busy, last_rec, any_verify);
        CHECK(up_err_is(400, "bad_request") && up_says("ended early") && g_ota_aborts == 1 && g_ota_ends == 0 && up_is("idle", -1, "", "bad_request") && !s_ota_busy && last_rec == 40 && !any_verify && g_ota_boot_set == NULL);
    }
    verdict("up8", "a body that ends early: 400 bad_request \"ended early\", the slot aborted once, never ended, the percent marks stopped at the last 5-step (40 %), the object idle with error bad_request");

    /* up9. busy: by hand, and for real (a second request arrives while the first one's body is being received) */
    {   up_boot(); s_ota_busy = true; int m0 = g_nmarks; int c = up_good(12000);
        printf("up9 flag set by hand, /update: http=%d %s\n", c, up_r.response);
        CHECK(up_err_is(409, "busy") && up_untouched() && up_is("idle", -1, "", "") && up_marks_from(m0) == 0 && s_ota_busy);
        httpd_req_t q; req_init(&q); q.body = "0123456789"; q.body_len = q.content_len = 10; int save = g_api_call; g_api_call = 0; ota_post(&q); g_api_call = save;
        printf("up9 flag set by hand, /ota: http=%d '%s'\n", q.code, q.response);
        CHECK(q.code == 409 && strstr(q.response, "already in progress") && q.response[0] != '{' && up_untouched());
        /* for real: the 3rd httpd_req_recv of the first request runs another /ota and another /update */
        up_boot(); up_hook_ran = 0; up_hook_ota_code = up_hook_upd_code = 0; up_hook_ota_text[0] = up_hook_upd_text[0] = 0; g_recv_hook = up_hook_other_requests; g_recv_hook_at = 3;
        c = up_good(20000);
        printf("up9 while an /update is receiving: it answers %d; a second /ota in the middle: %d '%.40s'; a second /update in the middle: %d %.60s; slots begun=%d; busy after=%d\n",
               c, up_hook_ota_code, up_hook_ota_text, up_hook_upd_code, up_hook_upd_text, g_ota_begins, s_ota_busy);
        CHECK(up_hook_ran == 1 && c == 200 && up_hook_ota_code == 409 && strstr(up_hook_ota_text, "already in progress") && up_hook_ota_text[0] != '{');
        CHECK(up_hook_upd_code == 409 && strstr(up_hook_upd_text, "\"error\":\"busy\"") && g_ota_begins == 1 && g_ota_ends == 1 && g_ota_aborts == 0 && g_ota_bytes == 20000);
        CHECK(s_ota_busy && up_is("rebooting", 100, "0.6.0", ""));                     /* a good update keeps the flag until the restart */
        c = up_good(12000);
        printf("up9 after a good update, a further /update: http=%d %s\n", c, up_r.response);
        CHECK(up_err_is(409, "busy") && g_ota_begins == 1);
    }
    verdict("up9", "busy: with the flag set /update answers 409 busy and /ota its own plain-text 409, nothing begun; and while an /update is really receiving, a second /ota and a second /update are refused, the first completes; a good update keeps the flag");

    /* up10. the percent cadence */
    {   static const size_t sizes[2] = {100000, 1466176};
        for (int k = 0; k < 2; k++) {
            up_boot(); int m0 = g_nmarks; int c = up_good(sizes[k]);
            int exp[300], got[300]; int ne = up_expect_pcts(sizes[k], exp), ng = up_pcts(m0, got);
            int same = ne == ng, nondec = 1, steps = 1; for (int i = 0; same && i < ne; i++) same &= exp[i] == got[i];
            for (int i = 0; i < ng; i++) { if (i && got[i] < got[i - 1]) nondec = 0; if (i && i < ng - 1 && got[i] - got[i - 1] < 5) steps = 0; }
            printf("up10 %zu B: http=%d; marks while receiving after the entry mark: %d (the rule gives %d); first %d, last %d; non-decreasing=%d, steps >= 5 but the last=%d; identical to the rule's list=%d; all marks %d\n",
                   sizes[k], c, ng, ne, ng ? got[0] : -1, ng ? got[ng - 1] : -1, nondec, steps, same, up_marks_from(m0));
            CHECK(c == 200 && ng == ne && same && nondec && steps && got[ng - 1] == 100 && up_marks_from(m0) == 1 + ng + 2);
            CHECK(k == 0 ? ne == 13 : ne == 20);
        }
    }
    verdict("up10", "the percent cadence: a mark each time the integer percent is 5 or more above the last, and at 100; 100,000 B in 4,096 B chunks gives 13 marks after the entry mark, a 1,466,176 B image 20, each list equal to the rule's, then verifying and rebooting");

    /* up11. flash and boot-slot failures */
    {   const char *what[5] = {"esp_ota_begin", "esp_ota_write", "esp_ota_set_boot_partition", "mbedtls_sha256", "esp_ota_write (chunk 1)"};
        const int aborts[5] = {0, 1, 0, 0, 1};
        for (int k = 0; k < 5; k++) {
            up_boot(); char h[65]; snprintf(h, sizeof h, "%s", up_hdr_of(20000));
            if (k == 0) g_ota_begin_err = 1; else if (k == 1) g_ota_write_err_at = 3; else if (k == 2) g_ota_boot_err = 1; else if (k == 3) g_sha_inc_fail = -0x0037; else g_ota_write_err_at = 1;
            int c = up_send(20000, 20000, h);
            printf("up11 %s fails: http=%d %s; aborts=%d (expected %d); ends=%d; boot set=%d; object %s/%s; busy=%d\n", what[k], c, up_r.response, g_ota_aborts, aborts[k], g_ota_ends, g_ota_boot_set != NULL, up_u.state, up_u.error, s_ota_busy);
            CHECK(up_err_is(500, "internal") && up_says(k == 4 ? "esp_ota_write" : what[k]) && g_ota_aborts == aborts[k] && up_is("idle", -1, "", "internal") && !s_ota_busy && g_ota_boot_set == NULL && g_closes == 0 && tasks_named("reboot") == 0);
        }
    }
    verdict("up11", "esp_ota_begin / esp_ota_write (chunk 3 and chunk 1) / esp_ota_set_boot_partition / the SHA-256 failing: 500 internal naming the call, aborted where a handle was open, the boot slot untouched, the object idle with error internal, the busy flag clear");

    /* up12. the object as JSON (st_update) */
    {   static char buf[512]; int len;
        up_boot(); cJSON *j = st_obj(ST_UPDATE, buf, sizeof buf, &len); const cJSON *u = j ? st_get(j, "update") : NULL; char *s0 = u ? cJSON_PrintUnformatted(u) : NULL;
        printf("up12 fresh: %s\n", s0 ? s0 : "(none)");
        CHECK(s0 && !strcmp(s0, "{\"state\":\"idle\",\"version\":null,\"percent\":null,\"error\":null}")); free(s0); cJSON_Delete(j);
        up_boot(); up_good(12000); j = st_obj(ST_UPDATE, buf, sizeof buf, &len); u = j ? st_get(j, "update") : NULL; s0 = u ? cJSON_PrintUnformatted(u) : NULL;
        printf("up12 after a good update: %s\n", s0 ? s0 : "(none)");
        CHECK(s0 && !strcmp(s0, "{\"state\":\"rebooting\",\"version\":\"0.6.0\",\"percent\":100,\"error\":null}")); free(s0); cJSON_Delete(j);
        up_boot(); g_ota_end_err = 5; up_good(12000); j = st_obj(ST_UPDATE, buf, sizeof buf, &len); u = j ? st_get(j, "update") : NULL; s0 = u ? cJSON_PrintUnformatted(u) : NULL;
        printf("up12 after a refused image (bad_image): %s\n", s0 ? s0 : "(none)");
        CHECK(s0 && !strcmp(s0, "{\"state\":\"idle\",\"version\":null,\"percent\":null,\"error\":\"bad_image\"}")); free(s0); cJSON_Delete(j);
        up_boot(); g_ota_end_err = 5; up_good(12000); g_ota_end_err = 0; up_good(12000); up_info();
        printf("up12 a good update after a refused one: error '%s' (the new attempt clears the old error)\n", up_u.error);
        CHECK(!strcmp(up_u.state, "rebooting") && !up_u.error[0]);
    }
    verdict("up12", "the update object in the state object: fresh {idle, null, null, null} (0.5.2's bytes); after a good update {rebooting, 0.6.0, 100, null}; after a refused image {idle, null, null, bad_image}; a new attempt clears the old error");

    /* up13. authentication */
    {   up_boot(); g_auto_bearer = 0; int m0 = g_nmarks; int c = up_good(12000);
        char code[32]; int st = au_code(&up_r, code, sizeof code);
        printf("up13 no Authorization on a claimed board: %d %s; touched=%d marks=%d busy=%d\n", c, code, !up_untouched(), up_marks_from(m0), s_ota_busy);
        CHECK(st == 401 && !strcmp(code, "unauthorized") && up_untouched() && up_marks_from(m0) == 0 && !s_ota_busy && up_is("idle", -1, "", ""));
        up_boot(); au_state(false, false); g_auto_bearer = 1; c = up_good(12000); st = au_code(&up_r, code, sizeof code);
        printf("up13 unclaimed board: %d %s; touched=%d\n", c, code, !up_untouched());
        CHECK(st == 403 && !strcmp(code, "claim_required") && up_untouched() && up_is("idle", -1, "", "") && !s_ota_busy);
        up_boot(); up_r.h_auth = NULL; req_init(&up_r); up_r.h_auth = "Bearer nottheone-zq9"; up_r.body = (const char *)up_body; up_r.body_len = up_r.content_len = 12000; up_r.h_sha = up_hdr_of(12000); api_call(update_post, &up_r);
        st = au_code(&up_r, code, sizeof code);
        printf("up13 the wrong token: %d %s\n", st, code);
        CHECK(st == 401 && up_untouched() && !s_ota_busy);
    }
    verdict("up13", "no token on a claimed board: 401 unauthorized; an unclaimed board: 403 claim_required; a wrong token: 401; nothing touched in any");
}
