/* fl1-fl4, rb1: the close flush of events.c (c6_app_events_close sends the keys marked and not yet sent, then ends the stream) and the restart delay of c6_base.c (reboot_task).
 * Uses ev_boot() and ev_open() of board_ev.c. */
static int fl_frames(const char *from, const char *name) { char pat[40]; snprintf(pat, sizeof pat, "event: %s\n", name); return str_count(from, pat); }
static int fl_rb_closes_at_delay, fl_rb_restarts_at_delay, fl_rb_stream_open_at_delay;
static void fl_rb_hook(void) { fl_rb_restarts_at_delay = g_restarts; fl_rb_closes_at_delay = g_closes; fl_rb_stream_open_at_delay = s_stream != NULL; }

static void probe_fl(void) {
    puts("==== fl: the close flush and the restart delay ====");
    /* fl1. marks made since the last event go out before the end */
    {   ev_boot(); au_state(true, false); g_auto_bearer = 1; httpd_req_t r; ev_open(&r); g_chunk_want = s_stream; uint32_t id0 = s_ev_id; size_t c0 = g_chunks_len; int n0 = g_chunks_n; size_t m = g_loglen;
        state_mark(ST_UPDATE); state_mark(ST_UPDATE); uint32_t dirty_before = s_dirty;
        c6_app_events_close();
        const char *t = g_chunks + c0; const char *ch = strstr(t, "event: change\n"), *en = strstr(t, "event: end\n\n");
        char pre[64]; snprintf(pre, sizeof pre, "id: %u\nevent: change\ndata: ", id0 + 1);
        printf("fl1 two ST_UPDATE marks, no events_step, then the close: dirty before=0x%x, chunks +%d (change events %d, end events %d), the change comes first=%d with id %u and only the update key=%d, dirty after=0x%x, completed=%d, stream open=%d, ended-by-the-board lines=%d, s_ev_id %u -> %u\n",
               dirty_before, g_chunks_n - n0, fl_frames(t, "change"), fl_frames(t, "end"), ch && en && ch < en, id0 + 1, ev_keys_are(t, "update", NULL), s_dirty, g_async_done, s_stream != NULL, log_count(m, "EVENTS stream ended by the board"), id0, s_ev_id);
        CHECK(dirty_before == ST_UPDATE && g_chunks_n - n0 == 3 && fl_frames(t, "change") == 1 && fl_frames(t, "end") == 1 && ch && en && ch < en && !strncmp(t, pre, strlen(pre)) && ev_keys_are(t, "update", NULL)
              && s_dirty == 0 && g_async_done == 1 && !s_stream && log_count(m, "EVENTS stream ended by the board") == 1 && s_ev_id == id0 + 1 && g_chunk_wrong == 0);
    }
    verdict("fl1", "the close flushes first: with marks not yet sent, exactly one 'event: change' (the open's id + 1, the marked key's JSON) and then 'event: end', s_dirty cleared, the request completed once, the stream gone");

    /* fl2. no stream: nothing is sent or cleared */
    {   ev_boot(); au_state(true, false); state_mark(ST_UPDATE); uint32_t dirty = s_dirty; int n0 = g_chunks_n, d0 = g_async_done; size_t m = g_loglen;
        c6_app_events_close();
        printf("fl2 a mark with no stream open, then the close: chunks %d -> %d, completed %d -> %d, dirty 0x%x -> 0x%x, log lines=%d\n", n0, g_chunks_n, d0, g_async_done, dirty, s_dirty, (int)(g_loglen - m));
        CHECK(g_chunks_n == n0 && g_async_done == d0 && s_dirty == dirty && (dirty & ST_UPDATE) && g_loglen == m);
    }
    verdict("fl2", "the close with no stream open sends nothing, completes nothing, keeps the marked keys and logs nothing");

    /* fl3. nothing marked since the open: only the end (ev3's close case) */
    {   ev_boot(); au_state(true, false); g_auto_bearer = 1; httpd_req_t r; ev_open(&r); g_chunk_want = s_stream; uint32_t id0 = s_ev_id; size_t c0 = g_chunks_len; int n0 = g_chunks_n;
        c6_app_events_close();
        const char *t = g_chunks + c0;
        printf("fl3 nothing marked since the open, then the close: chunks +%d, change events %d, 'event: end' first=%d, completed=%d, s_ev_id %u -> %u\n", g_chunks_n - n0, fl_frames(t, "change"), !strncmp(t, "event: end\n\n", 12), g_async_done, id0, s_ev_id);
        CHECK(g_chunks_n - n0 == 2 && fl_frames(t, "change") == 0 && !strncmp(t, "event: end\n\n", 12) && g_async_done == 1 && !s_stream && s_ev_id == id0 && s_dirty == 0 && g_chunk_wrong == 0);
    }
    verdict("fl3", "the close with nothing marked since the open sends only 'event: end' (no change event) and leaves the event id alone");

    /* fl4. the flush's send fails */
    {   ev_boot(); au_state(true, false); g_auto_bearer = 1; httpd_req_t r; ev_open(&r); g_chunk_want = s_stream; size_t c0 = g_chunks_len; size_t m = g_loglen;
        state_mark(ST_UPDATE); g_chunk_fail_at = g_chunks_n + 1;
        c6_app_events_close();
        const char *t = g_chunks + c0; int n1 = g_chunks_n, d1 = g_async_done;
        printf("fl4 the flush send fails: 'event: end' sent=%d, completed=%d, stream open=%d, dirty=0x%x, dropped lines=%d\n", strstr(t, "event: end") != NULL, d1, s_stream != NULL, s_dirty, log_count(m, "EVENTS stream dropped: a send failed"));
        CHECK(!strstr(t, "event: end") && d1 == 1 && !s_stream && s_dirty == 0 && log_count(m, "EVENTS stream dropped: a send failed") == 1);
        c6_app_events_close();
        printf("fl4 a second close: chunks %d -> %d, completed %d -> %d\n", n1, g_chunks_n, d1, g_async_done);
        CHECK(g_chunks_n == n1 && g_async_done == d1 && g_chunk_wrong == 0);
    }
    verdict("fl4", "when the flush's send fails the stream is dropped by the send (logged), no 'event: end' follows, the request is completed once, and a second close does nothing");

    /* rb1. the restart waits REBOOT_DELAY_MS after the stream is closed */
    {   ev_boot(); au_state(true, false); g_auto_bearer = 1; httpd_req_t r; ev_open(&r); g_chunk_want = s_stream;
        fl_rb_closes_at_delay = fl_rb_restarts_at_delay = -1; fl_rb_stream_open_at_delay = -1; int cl0 = g_closes, rs0 = g_restarts, dl0 = g_delays; int64_t t0 = fake_now;
        g_delay_hook = fl_rb_hook; reboot_task(NULL); g_delay_hook = NULL;
        int64_t waited = fake_now - t0;
        printf("rb1 reboot_task(NULL): waited %lld us (REBOOT_DELAY_MS=%d), delays +%d; at the delay: closes +%d, restarts %d, stream open=%d; after it: restarts +%d, closes +%d\n",
               (long long)waited, REBOOT_DELAY_MS, g_delays - dl0, fl_rb_closes_at_delay - cl0, fl_rb_restarts_at_delay - rs0, fl_rb_stream_open_at_delay, g_restarts - rs0, g_closes - cl0);
        CHECK(REBOOT_DELAY_MS == 2000 && waited == 2000000 && g_delays - dl0 == 1 && fl_rb_closes_at_delay - cl0 == 1 && fl_rb_restarts_at_delay == rs0 && fl_rb_stream_open_at_delay == 0 && g_restarts - rs0 == 1 && g_closes - cl0 == 1);
    }
    verdict("rb1", "reboot_task closes the event stream first, then waits exactly REBOOT_DELAY_MS (2,000 ms, read from the firmware) before esp_restart");
}
