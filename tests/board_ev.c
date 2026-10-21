/* ev1-ev4: events.c (GET /api/v1/events, the events task's step on fake time, the close hooks). */
static void ev_boot(void) { reset_case(); events_start(); g_auto_bearer = 1; state_mark(ST_LIGHT); state_mark(ST_LIGHT); state_mark(ST_LIGHT); }
static int ev_open(httpd_req_t *r) { req_init(r); api_call(events_get, r); return r->code; }
static const char *ev_tail(size_t from) { return g_chunks + from; }
static int ev_keys_are(const char *frame, const char *k1, const char *k2) {          /* the data object of an event has exactly these top-level keys */
    const char *d = strstr(frame, "data: "); if (!d) return 0; cJSON *j = cJSON_Parse(d + 6); int n = 0, ok = j != NULL;
    for (const cJSON *c = j ? j->child : NULL; c; c = c->next) { n++; ok &= !strcmp(c->string, k1) || (k2 && !strcmp(c->string, k2)); }
    cJSON_Delete(j); return ok && n == (k2 ? 2 : 1);
}

static void probe_ev(void) {
    puts("==== ev: the stream ====");
    /* ev1. open, change, refresh */
    {   ev_boot(); httpd_req_t r; static char exp[2048], want[4096]; int n = state_json(exp, sizeof exp, ST_ALL); uint32_t seq0 = s_seq; size_t c0 = g_chunks_len;
        int code = ev_open(&r); httpd_req_t *a = &g_async_pool[0];
        snprintf(want, sizeof want, "id: %u\nevent: state\ndata: %s\n\n", seq0, exp);
        printf("ev1 open: begin calls=%d, type=%s, Cache-Control=%s, first chunk exactly the state event (id %u, %d data bytes)=%d, chunks=%d, dirty=%u\n", g_async_begun, a->type, resp_hdr(a, "Cache-Control") ? resp_hdr(a, "Cache-Control") : "?", seq0, n, !strcmp(ev_tail(c0), want), g_chunks_n, s_dirty);
        CHECK(code == 0 && g_async_begun == 1 && !strcmp(a->type, "text/event-stream") && resp_hdr(a, "Cache-Control") && !strcmp(resp_hdr(a, "Cache-Control"), "no-cache") && !strcmp(ev_tail(c0), want) && g_chunks_n == 1 && s_dirty == 0 && s_stream == a && g_async_done == 0);
        state_mark(ST_LIGHT); state_mark(ST_LIGHT | ST_HUB); size_t c1 = g_chunks_len; events_step(fake_now);
        char pre[64]; snprintf(pre, sizeof pre, "id: %u\nevent: change\ndata: ", seq0 + 1);
        printf("ev1 two marks before the task ran: one change event id %u (the state's id + 1), keys light+Zigbee controller=%d, dirty=%u, chunks=%d\n", seq0 + 1, ev_keys_are(ev_tail(c1), "light", "controller"), s_dirty, g_chunks_n);
        CHECK(!strncmp(ev_tail(c1), pre, strlen(pre)) && ev_keys_are(ev_tail(c1), "light", "controller") && s_dirty == 0 && g_chunks_n == 2);
        fake_now += 60000000; size_t c2 = g_chunks_len; events_step(fake_now);
        snprintf(pre, sizeof pre, "id: %u\nevent: change\ndata: ", seq0 + 2);
        printf("ev1 60 s refresh with nothing marked: id %u, keys board+zigbee=%d\n", seq0 + 2, ev_keys_are(ev_tail(c2), "board", "zigbee"));
        CHECK(!strncmp(ev_tail(c2), pre, strlen(pre)) && ev_keys_are(ev_tail(c2), "board", "zigbee") && g_chunks_n == 3);
        int cn = g_chunks_n; events_step(fake_now); state_mark(ST_BUTTON); size_t c3 = g_chunks_len; events_step(fake_now);
        snprintf(pre, sizeof pre, "id: %u\nevent: change\ndata: ", seq0 + 3);
        printf("ev1 nothing marked: nothing sent (chunks %d -> %d); a later mark gets the next id %u: %d\n", cn, g_chunks_n - 1, seq0 + 3, !strncmp(ev_tail(c3), pre, strlen(pre)));
        CHECK(cn == 3 && g_chunks_n == 4 && !strncmp(ev_tail(c3), pre, strlen(pre)) && ev_keys_are(ev_tail(c3), "button", NULL));
    }
    verdict("ev1", "GET /events: one async begin, text/event-stream, no-cache, the first chunk exactly id+event: state+the whole object; two marks before the task runs give one change with the union of keys and the state's id + 1; a 60 s refresh sends board and zigbee with the next id; nothing marked, nothing sent");

    /* ev2. ping and refresh timing */
    {   ev_boot(); httpd_req_t r; ev_open(&r); int64_t T = fake_now; size_t c0 = g_chunks_len; int cn = g_chunks_n;
        fake_now = T + 14900000; events_step(fake_now); int none149 = g_chunks_n == cn;
        fake_now = T + 15000000; events_step(fake_now); int ping15 = !strcmp(ev_tail(c0), ": ping\n\n");
        fake_now = T + 30000000; events_step(fake_now); fake_now = T + 45000000; events_step(fake_now); int pings = g_chunks_n - cn;
        fake_now = T + 60000000; size_t c1 = g_chunks_len; events_step(fake_now);
        printf("ev2 idle: nothing at 14.9 s=%d, ': ping' at 15 s=%d; pings at 15/30/45 s=%d; at 60 s a change with board+zigbee only=%d (no ping)\n", none149, ping15, pings, ev_keys_are(ev_tail(c1), "board", "zigbee"));
        CHECK(none149 && ping15 && pings == 3 && ev_keys_are(ev_tail(c1), "board", "zigbee") && !strstr(ev_tail(c1), "ping"));
        ev_boot(); ev_open(&r); T = fake_now; fake_now = T + 14900000; events_step(fake_now);
        fake_now = T + 20000000; state_mark(ST_LIGHT); size_t c2 = g_chunks_len; int cn2 = g_chunks_n; events_step(fake_now); int chg = strstr(ev_tail(c2), "event: change") != NULL;
        fake_now = T + 34900000; events_step(fake_now); int none349 = g_chunks_n == cn2 + 1;
        fake_now = T + 35000000; size_t c3 = g_chunks_len; events_step(fake_now); int ping35 = !strcmp(ev_tail(c3), ": ping\n\n");
        printf("ev2 a change at 20 s: sent=%d; no ping at 34.9 s=%d; ping at 35.0 s (15 s after the change)=%d\n", chg, none349, ping35);
        CHECK(chg && none349 && ping35);
        ev_boot(); ev_open(&r); T = fake_now; c6_app_events_close(); long cj0 = g_cj_allocs; int cn3 = g_chunks_n; fake_now = T + 60000000; uint32_t w = events_step(fake_now);
        printf("ev2 60 s with no stream: nothing built (cJSON allocations +%ld) or sent (chunks +%d); the task waits forever=%d\n", g_cj_allocs - cj0, g_chunks_n - cn3, w == UINT32_MAX);
        CHECK(g_cj_allocs == cj0 && g_chunks_n == cn3 && w == UINT32_MAX);
    }
    verdict("ev2", "idle time: nothing at 14.9 s, a ping at 15 s only and counted again from a change; at 60 s a change with board and zigbee only; with no stream open nothing is built or sent");

    /* ev3. replacement, failure, close hooks */
    {   ev_boot(); httpd_req_t r1, r2; ev_open(&r1); httpd_req_t *s1 = s_stream; size_t c0 = g_chunks_len; ev_open(&r2); httpd_req_t *s2 = s_stream;
        const char *endp = strstr(g_chunks + c0, "event: end\n\n");
        printf("ev3 a second GET: begin calls=%d, the old stream got 'event: end'=%d and was completed %d time(s); one stream open, the new one=%d\n", g_async_begun, endp != NULL, g_async_done, s2 != s1 && s2 == &g_async_pool[1]);
        CHECK(g_async_begun == 2 && endp && g_async_done == 1 && s2 == &g_async_pool[1] && s1 != s2 && strstr(endp + 12, "event: state"));
        g_chunk_fail_at = g_chunks_n + 1; state_mark(ST_LIGHT); events_step(fake_now); int done_after_fail = g_async_done; int open_after_fail = s_stream != NULL;
        int cn = g_chunks_n; state_mark(ST_LIGHT); events_step(fake_now);
        printf("ev3 a failed send: completed once more (total %d), stream dropped=%d; the next mark sends nothing (chunk calls %d -> %d)\n", done_after_fail, !open_after_fail, cn, g_chunks_n);
        CHECK(done_after_fail == 2 && !open_after_fail && g_chunks_n == cn && g_async_done == 2);
        int d0 = g_async_done, n0 = g_chunks_n; c6_app_events_close(); CHECK(g_async_done == d0 && g_chunks_n == n0);
        ev_boot(); ev_open(&r1); size_t c1 = g_chunks_len; c6_app_events_close();
        printf("ev3 close on an open idle stream: 'event: end'=%d, completed=%d, stream open after=%d; closed again: completed still %d\n", strstr(g_chunks + c1, "event: end\n\n") != NULL, g_async_done, s_stream != NULL, (c6_app_events_close(), g_async_done));
        CHECK(strstr(g_chunks + c1, "event: end\n\n") && g_async_done == 1 && !s_stream);
        for (int k = 0; k < 3; k++) {
            ev_boot(); au_state(true, false); g_auto_bearer = 1; ev_open(&r1); size_t c2 = g_chunks_len; int cl0 = g_closes; httpd_req_t q; req_init(&q);
            if (k == 0) c6_auth_clear(); else if (k == 1) api_call(api_release_post, &q); else api_call(api_reboot_post, &q);
            printf("ev3 %s: events_close calls +%d, the stream ended='event: end'=%d, completed=%d\n", k == 0 ? "clear" : k == 1 ? "release" : "reboot", g_closes - cl0, strstr(g_chunks + c2, "event: end\n\n") != NULL, g_async_done);
            CHECK(g_closes - cl0 == 1 && strstr(g_chunks + c2, "event: end\n\n") && g_async_done == 1 && !s_stream);
        }
    }
    verdict("ev3", "a second GET ends the first with 'event: end' and completes it once; a failed send closes and completes once and later marks send nothing; events_close is a no-op with none open; clear, release and reboot each close the open stream once");

    /* ev4. authorisation before the stream */
    {   httpd_req_t r; char code[32];
        ev_boot(); au_state(true, false); ev_open(&r); int st = au_code(&r, code, sizeof code);
        printf("ev4 claimed, no token: %d %s; async begins=%d\n", st, code, g_async_begun);
        CHECK(st == 401 && !strcmp(code, "unauthorized") && g_async_begun == 0 && !strcmp(r.type, "application/json"));
        ev_boot(); au_state(true, false); req_init(&r); r.h_auth = "Bearer wrong-zq9"; api_call(events_get, &r); st = au_code(&r, code, sizeof code);
        printf("ev4 claimed, wrong token: %d %s; async begins=%d\n", st, code, g_async_begun);
        CHECK(st == 401 && g_async_begun == 0);
        ev_boot(); au_state(false, true); req_init(&r); r.h_auth = G_BEARER; api_call(events_get, &r); st = au_code(&r, code, sizeof code);
        printf("ev4 unclaimed with a Bearer: %d %s; async begins=%d\n", st, code, g_async_begun);
        CHECK(st == 403 && !strcmp(code, "claim_required") && g_async_begun == 0);
        ev_boot(); au_state(false, true); ev_open(&r);
        printf("ev4 unclaimed, window open, no credential: async begins=%d, stream open=%d\n", g_async_begun, s_stream != NULL);
        CHECK(g_async_begun == 1 && s_stream != NULL);
        ev_boot(); au_state(false, false); ev_open(&r); st = au_code(&r, code, sizeof code);
        printf("ev4 unclaimed, window closed: %d %s; async begins=%d\n", st, code, g_async_begun);
        CHECK(st == 403 && g_async_begun == 0);
    }
    verdict("ev4", "GET /events claimed with no or a wrong token is 401, unclaimed with a Bearer 403 claim_required, as JSON and with no stream started; unclaimed in the window without a credential starts one");
}
