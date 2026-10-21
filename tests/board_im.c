/* im1-im5: improv.c. improv_task is stepped by the USB read stub (a byte queue released by fake time; a quota of reads, then the loop is left);
 * what it wrote is cut into Improv packets, every checksum checked. The password is the made-up WF_PASS. */
static struct {int type, len, ok; uint8_t d[256];} im_pk[96]; static int im_npk, im_stray;
static int im_cut(size_t from) {
    im_npk = 0; im_stray = 0; size_t i = from;
    while (i < g_usb_out_len) {
        if (g_usb_out_len - i >= 9 && !memcmp(g_usb_out + i, "IMPROV", 6)) {
            size_t l = g_usb_out[i + 8];
            if (i + 9 + l + 2 > g_usb_out_len) { im_stray++; break; }
            uint8_t s = 0; for (size_t k = 0; k < 9 + l; k++) s = (uint8_t)(s + g_usb_out[i + k]);
            if (im_npk < 96) {
                im_pk[im_npk].type = g_usb_out[i + 7]; im_pk[im_npk].len = (int)l;
                im_pk[im_npk].ok = g_usb_out[i + 6] == 1 && s == g_usb_out[i + 9 + l] && g_usb_out[i + 9 + l + 1] == '\n';
                memcpy(im_pk[im_npk].d, g_usb_out + i + 9, l); im_npk++;
            }
            i += 9 + l + 2;
        } else { im_stray++; i++; }
    }
    return im_npk;
}
static int im_all_ok(void) { for (int k = 0; k < im_npk; k++) if (!im_pk[k].ok) return 0; return im_stray == 0; }
static char im_text[1200];
static const char *im_desc(void) {                       /* S<state> E<error> R<cmd>[str|str|...]; a malformed result is shown as R? */
    size_t n = 0; im_text[0] = 0;
    for (int k = 0; k < im_npk && n + 100 < sizeof im_text; k++) {
        const uint8_t *d = im_pk[k].d; int l = im_pk[k].len;
        n += (size_t)snprintf(im_text + n, sizeof im_text - n, "%s", k ? " " : "");
        if (im_pk[k].type == 1 && l == 1) n += (size_t)snprintf(im_text + n, sizeof im_text - n, "S%d", d[0]);
        else if (im_pk[k].type == 2 && l == 1) n += (size_t)snprintf(im_text + n, sizeof im_text - n, "E%d", d[0]);
        else if (im_pk[k].type == 4 && l >= 2 && d[1] == l - 2) {
            n += (size_t)snprintf(im_text + n, sizeof im_text - n, "R%d[", d[0]); int p = 2, first = 1, good = 1;
            while (p < l) { int sl = d[p]; if (p + 1 + sl > l) { good = 0; break; } n += (size_t)snprintf(im_text + n, sizeof im_text - n, "%s%.*s", first ? "" : "|", sl, (const char *)d + p + 1); first = 0; p += 1 + sl; }
            n += (size_t)snprintf(im_text + n, sizeof im_text - n, "%s]", good ? "" : "?");
        } else n += (size_t)snprintf(im_text + n, sizeof im_text - n, "?%d", im_pk[k].type);
    }
    return im_text;
}
static int im_has(const uint8_t *h, size_t hn, const char *needle) { size_t nl = strlen(needle); for (size_t i = 0; i + nl <= hn; i++) if (!memcmp(h + i, needle, nl)) return 1; return 0; }
static int im_leaks(void) {                               /* the password anywhere it must not be: the port, the log, the parser's buffer */
    return im_has(g_usb_all, g_usb_all_len, "pw-zq9") || im_has((const uint8_t *)g_logbuf, strlen(g_logbuf), "pw-zq9") || im_has(s_improv.raw, sizeof s_improv.raw, "pw-zq9");
}
/* frames */
static size_t im_frame(uint8_t *o, uint8_t ver, uint8_t type, const uint8_t *d, size_t len, int sum_add) {
    memcpy(o, "IMPROV", 6); o[6] = ver; o[7] = type; o[8] = (uint8_t)len; if (len) memcpy(o + 9, d, len);
    uint8_t s = 0; for (size_t i = 0; i < 9 + len; i++) s = (uint8_t)(s + o[i]); o[9 + len] = (uint8_t)(s + sum_add); return 9 + len + 1;
}
static size_t im_rpc(uint8_t *o, uint8_t cmd, const uint8_t *pl, size_t plen) {      /* a valid command frame */
    uint8_t d[200]; d[0] = cmd; d[1] = (uint8_t)plen; if (plen) memcpy(d + 2, pl, plen); return im_frame(o, 1, 3, d, plen + 2, 0);
}
static size_t im_rpc_lie(uint8_t *o, uint8_t cmd, const uint8_t *pl, size_t plen, uint8_t declared) {   /* the RPC's own length byte disagrees with the frame */
    uint8_t d[200]; d[0] = cmd; d[1] = declared; if (plen) memcpy(d + 2, pl, plen); return im_frame(o, 1, 3, d, plen + 2, 0);
}
static size_t im_c1(uint8_t *p, const void *ssid, int sl_actual, int sl_decl, const void *pass, int pl_actual, int pl_decl) {   /* command 1's data */
    size_t n = 0; p[n++] = (uint8_t)sl_decl; memcpy(p + n, ssid, (size_t)sl_actual); n += (size_t)sl_actual; p[n++] = (uint8_t)pl_decl; memcpy(p + n, pass, (size_t)pl_actual); n += (size_t)pl_actual; return n;
}
static void im_put(const void *p, size_t n, int64_t delay_us) { for (size_t i = 0; i < n && g_usb_in_n < 4096; i++) { g_usb_in[g_usb_in_n].b = ((const uint8_t *)p)[i]; g_usb_in[g_usb_in_n++].at = fake_now + delay_us; } }
static void im_puts(const char *s, int64_t delay_us) { im_put(s, strlen(s), delay_us); }
static void im_put_rpc(uint8_t cmd, int64_t delay_us) { uint8_t f[40]; size_t n = im_rpc(f, cmd, NULL, 0); im_put(f, n, delay_us); }
static void im_boot(void) { reset_case(); c6_wifi_start(); }
static void im_run(int reads) { g_usb_reads_left = reads; if (!setjmp(loop_done)) improv_task(NULL); g_usb_reads_left = 0; g_usb_each = NULL; }
/* a scripted event driver: events at fake-time offsets from im_t0 (1 got IP, 2 disconnect), and a record of the pending bytes at each read */
static int64_t im_t0; static struct {int64_t at; int what; int done;} im_ev[4]; static int im_nev;
static int im_pend_max, im_pend_end; static size_t im_len_at[500]; static int64_t im_t_at[500];
static void im_each(int r) {
    for (int i = 0; i < im_nev; i++) if (!im_ev[i].done && fake_now >= im_t0 + im_ev[i].at) { im_ev[i].done = 1; if (im_ev[i].what == 1) wf_got_ip(); else wf_disc(8); }
    if ((int)s_improv.n > im_pend_max) im_pend_max = (int)s_improv.n;
    im_pend_end = (int)s_improv.n; if (r < 500) { im_len_at[r] = g_usb_out_len; im_t_at[r] = fake_now; }
}
static void im_script(int64_t at1, int what1, int64_t at2, int what2) {
    im_t0 = fake_now; im_nev = 0; im_pend_max = im_pend_end = 0;
    if (what1) { im_ev[im_nev].at = at1; im_ev[im_nev].what = what1; im_ev[im_nev++].done = 0; }
    if (what2) { im_ev[im_nev].at = at2; im_ev[im_nev].what = what2; im_ev[im_nev++].done = 0; }
    g_usb_each = im_each;
}
#define IM_URLS "[http://c6-112233.local/|http://192.0.2.77/]"

static void probe_im(void) {
    puts("==== im: Improv over USB ====");
    /* im1. the parser, over a byte stream */
    {   uint8_t v[40], bad[40], ver2[40], ov[24], half[40], tmp[40]; size_t nv = im_rpc(v, 2, NULL, 0); size_t nb = im_frame(bad, 1, 3, (const uint8_t[]){2, 0}, 2, 1); size_t n2 = im_frame(ver2, 2, 3, (const uint8_t[]){2, 0}, 2, 0);
        memset(ov, 'x', sizeof ov); memcpy(ov, "IMPROV", 6); ov[6] = 1; ov[7] = 3; ov[8] = 200; size_t nh = im_rpc(tmp, 2, NULL, 0); memcpy(half, tmp, nh - 1);
        im_boot(); size_t stream = 0;
        const char *noise = "I (1234) wifi: IMPRO station started\r\n";
        im_puts(noise, 0); im_put(v, nv, 0); im_puts("\n", 0); im_puts(noise, 0); im_put(bad, nb, 0); im_put(v, nv, 0); im_puts(noise, 0); im_put(ver2, n2, 0);
        im_put(ov, sizeof ov, 0); im_put(half, nh - 1, 0); im_put(v, nv, 500000); im_puts(noise, 500000); stream = (size_t)g_usb_in_n;
        im_script(0, 0, 0, 0); im_run(60); im_cut(0);
        printf("im1 stream of %zu bytes (noise, valid, noise, bad checksum + valid, noise, version 2, length 200, a half frame, 500 ms quiet, valid, noise): packets=%d [%s], all checksums valid=%d, stray bytes=%d, most pending bytes=%d, pending at the end=%d, station sets=%d\n",
               stream, im_npk, im_desc(), im_all_ok(), im_stray, im_pend_max, im_pend_end, g_w_sta_sets);
        CHECK(!strcmp(im_desc(), "S2 E0 S2 E0 S2 E0 S2") && im_all_ok() && im_pend_max >= 11 && im_pend_end == 0 && g_w_sta_sets == 0 && g_w_connects == 0);
        im_boot(); im_put(bad, nb, 0); im_puts(noise, 0); im_put(ver2, n2, 0); im_put(ov, sizeof ov, 0); im_put(half, nh - 1, 0); im_put(ov, sizeof ov, 400000); im_script(0, 0, 0, 0); im_run(30); im_cut(0);
        printf("im1 only the bad frames (checksum off by one, version 2, length past the buffer, half, noise): packets=%d [%s], pending at the end=%d, station sets=%d\n", im_npk, im_desc(), im_pend_end, g_w_sta_sets);
        CHECK(!strcmp(im_desc(), "S2") && im_all_ok() && im_pend_end == 0 && g_w_sta_sets == 0 && g_w_connects == 0 && !s_improv_session);
        im_boot(); im_put(half, nh - 1, 0); im_script(0, 0, 0, 0); im_run(6); im_cut(0);
        printf("im1 a half frame then quiet: pending bytes seen=%d, then %d; packets=%d [%s]\n", im_pend_max, im_pend_end, im_npk, im_desc());
        CHECK(im_pend_max == (int)nh - 1 && im_pend_end == 0 && !strcmp(im_desc(), "S2"));
    }
    verdict("im1", "a valid command-2 frame is answered once (error 0, state); the bad-checksum, version-2 and oversize frames, noise and an abandoned half frame cause no output and no side effect (no error 1: this parser drops them silently); a frame after a bad one, or after a half one, is parsed");

    /* im2. command 1 */
    {   uint8_t p[120], f[160]; im_boot(); size_t m = g_loglen; size_t pn = im_c1(p, "zq9net", 6, 6, WF_PASS, 10, 10), fn = im_rpc(f, 1, p, pn); static size_t before; before = 0;
        im_put(f, fn, 0); im_script(5000000, 1, 0, 0); im_run(80); im_cut(0); (void)before;
        printf("im2 command 1 (zq9net, a made-up password), an IP after 5 s: packets=%d [%s], all checksums valid=%d, station pair as given=%d sets=%d connects=%d, c6_wifi state=%d, password echoed=%d, 'connecting to zq9net' log lines=%d\n",
               im_npk, im_desc(), im_all_ok(), w_sta_is("zq9net", WF_PASS), g_w_sta_sets, g_w_connects, (int)c6_wifi_state(), im_leaks(), log_count(m, "IMPROV Wi-Fi settings stored; connecting to zq9net"));
        CHECK(!strcmp(im_desc(), "S2 E0 S3 S4 R1" IM_URLS) && im_all_ok() && w_sta_is("zq9net", WF_PASS) && g_w_sta_sets == 1 && g_w_connects >= 1 && c6_wifi_state() == C6_WIFI_CONNECTED && !im_leaks() && s_improv_provisioning == 0);
        im_boot(); esp_netif_set_ip4_addr(&g_w_sta_ip_info.ip, 10, 1, 2, 3); im_put(f, fn, 0); im_script(1000000, 1, 0, 0); im_run(30); im_cut(0);
        CHECK(strstr(im_desc(), "http://10.1.2.3/") != NULL && strstr(im_desc(), "http://c6-112233.local/") != NULL);
        printf("im2 the same with the address 10.1.2.3: the second URL follows the netif: %d\n", strstr(im_desc(), "http://10.1.2.3/") != NULL);
    }
    verdict("im2", "command 1: c6_wifi_set gets exactly the ssid and password sent; state 3 first; at the IP, state 4 then the result for command 1 with http://c6-<id>.local/ and http://<ip>/; every checksum valid, the password never on the port, the log or in the parser");

    /* im3. command 1 with no IP, and bad command 1s */
    {   uint8_t p[120], f[160]; im_boot(); size_t pn = im_c1(p, "zq9net", 6, 6, WF_PASS, 10, 10), fn = im_rpc(f, 1, p, pn); int64_t t0 = fake_now;
        im_put(f, fn, 0); im_script(0, 0, 0, 0); im_run(330); im_cut(0); im_t0 = t0;
        int r30 = -1, r299 = -1; size_t after_s3 = 3 * 12; for (int r = 0; r < 330 && r < 500; r++) if (im_len_at[r] > after_s3) { r30 = r; break; }
        printf("im3 command 1, no IP: packets=%d [%s]; the error appeared at +%.1f s (the read before it, at +%.1f s, had %zu bytes of the first 3 packets); state 4 sent=%d, result sent=%d, password echoed=%d, c6_wifi state=%d\n", im_npk, im_desc(),
               r30 >= 0 ? (double)(im_t_at[r30] - t0) / 1e6 : -1.0, r30 > 0 ? (double)(im_t_at[r30 - 1] - t0) / 1e6 : -1.0, r30 > 0 ? im_len_at[r30 - 1] : (size_t)0, strstr(im_desc(), "S4") != NULL, strstr(im_desc(), "R") != NULL, im_leaks(), (int)c6_wifi_state());
        r299 = r30;
        CHECK(!strcmp(im_desc(), "S2 E0 S3 E3 S2") && r299 > 0 && im_t_at[r299] - t0 == 30000000 && im_len_at[r299 - 1] == after_s3 && im_all_ok() && !im_leaks() && !s_improv_provisioning);
        char ssid33[34]; memset(ssid33, 'a', 33); ssid33[33] = 0; uint8_t f2[200]; size_t n2;
        const struct {const char *what; const char *want;} cs[] = {{"ssid length 0", "S2 E0 E3 E0 S2"}, {"ssid length 33", "S2 E0 E3 E0 S2"}, {"a 5-character password", "S2 E0 E3 E0 S2"}, {"a NUL in the ssid", "S2 E0 E3 E0 S2"},
            {"a NUL in the password", "S2 E0 E3 E0 S2"}, {"ssid length declared past the frame", "S2 E0 E1 E0 S2"}, {"password length declared past the frame", "S2 E0 E1 E0 S2"}, {"the RPC length byte longer than the frame", "S2 E1 E0 S2"},
            {"the RPC length byte shorter than the frame", "S2 E1 E0 S2"}};
        for (int c = 0; c < 9; c++) {
            im_boot(); int c0 = g_w_connects, d0 = g_w_disconnects; size_t m = g_loglen;
            switch (c) {
            case 0: pn = im_c1(p, "", 0, 0, "", 0, 0); n2 = im_rpc(f2, 1, p, pn); break;
            case 1: pn = im_c1(p, ssid33, 33, 33, WF_PASS, 10, 10); n2 = im_rpc(f2, 1, p, pn); break;
            case 2: pn = im_c1(p, "zq9net", 6, 6, "pw-zq", 5, 5); n2 = im_rpc(f2, 1, p, pn); break;
            case 3: pn = im_c1(p, "zq9\0net", 7, 7, WF_PASS, 10, 10); n2 = im_rpc(f2, 1, p, pn); break;
            case 4: pn = im_c1(p, "zq9net", 6, 6, "pw-zq9\0net", 10, 10); n2 = im_rpc(f2, 1, p, pn); break;
            case 5: pn = im_c1(p, "zq9net", 6, 40, WF_PASS, 10, 10); n2 = im_rpc(f2, 1, p, pn); break;
            case 6: pn = im_c1(p, "zq9net", 6, 6, WF_PASS, 10, 20); n2 = im_rpc(f2, 1, p, pn); break;
            case 7: pn = im_c1(p, "zq9net", 6, 6, WF_PASS, 10, 10); n2 = im_rpc_lie(f2, 1, p, pn, (uint8_t)(pn + 12)); break;
            default: pn = im_c1(p, "zq9net", 6, 6, WF_PASS, 10, 10); n2 = im_rpc_lie(f2, 1, p, pn, (uint8_t)(pn - 3)); break;
            }
            im_put(f2, n2, 0); im_put_rpc(2, 300000); im_script(0, 0, 0, 0); im_run(12); im_cut(0);
            printf("im3 %s: packets=%d [%s]; connects +%d, disconnects +%d, station sets=%d, stored ssid=%d, password echoed=%d\n", cs[c].what, im_npk, im_desc(), g_w_connects - c0, g_w_disconnects - d0, g_w_sta_sets, fs_str("c6wifi", "ssid") != NULL, im_leaks());
            CHECK(!strcmp(im_desc(), cs[c].want) && im_all_ok() && g_w_connects == c0 && g_w_disconnects == d0 && g_w_sta_sets == 0 && !fs_str("c6wifi", "ssid") && !s_improv_provisioning && !im_leaks() && log_count(m, "pw-zq9") == 0);
        }
    }
    verdict("im3", "command 1 with no IP for 30 s: error 3 at exactly 30 s, then state 2, no state 4 and no result; a zero or 33-character ssid, a 5-character password, a NUL in either, a length past the frame or an RPC length that disagrees: no connect, nothing stored, an error packet (3 or 1), and the parser answers the next frame");

    /* im4. command 3 and 4 */
    {   im_boot(); im_put_rpc(3, 0); im_script(0, 0, 0, 0); im_run(8); im_cut(0);
        printf("im4 command 3, no name stored: [%s]\n", im_desc());
        CHECK(!strcmp(im_desc(), "S2 E0 R3[c6_bulb|0.5.0|ESP32-C6|Zigbee Anthema 112233]") && im_all_ok());
        im_boot(); kv_put_str("name", "zq9 light"); im_put_rpc(3, 0); im_script(0, 0, 0, 0); im_run(8); im_cut(0);
        printf("im4 command 3, name 'zq9 light' stored: [%s]\n", im_desc());
        CHECK(!strcmp(im_desc(), "S2 E0 R3[c6_bulb|0.5.0|ESP32-C6|zq9 light]") && im_all_ok());
        im_boot(); snprintf((char *)g_w_aps[0].ssid, 33, "zq9-a"); g_w_aps[0].rssi = -40; g_w_aps[0].authmode = WIFI_AUTH_WPA2_PSK; snprintf((char *)g_w_aps[1].ssid, 33, "zq9-b"); g_w_aps[1].rssi = -70; g_w_aps[1].authmode = WIFI_AUTH_OPEN;
        snprintf((char *)g_w_aps[2].ssid, 33, "zq9-c"); g_w_aps[2].rssi = -55; g_w_aps[2].authmode = WIFI_AUTH_WPA2_PSK; g_w_nap = 3; im_put_rpc(4, 0); im_script(0, 0, 0, 0); im_run(8); im_cut(0);
        printf("im4 command 4 over 3 networks: [%s]\n", im_desc());
        CHECK(!strcmp(im_desc(), "S2 E0 R4[zq9-a|-40|YES] R4[zq9-c|-55|YES] R4[zq9-b|-70|NO] R4[]") && im_all_ok());
        im_boot(); g_w_nap = 0; im_put_rpc(4, 0); im_script(0, 0, 0, 0); im_run(8); im_cut(0);
        printf("im4 command 4 over 0 networks: [%s]\n", im_desc()); CHECK(!strcmp(im_desc(), "S2 E0 R4[]") && im_all_ok());
        im_boot(); g_w_nap = 3; g_w_scan_err = ESP_FAIL; im_put_rpc(4, 0); im_script(0, 0, 0, 0); im_run(8); im_cut(0);
        printf("im4 command 4 with the scan failing: [%s]\n", im_desc()); CHECK(!strcmp(im_desc(), "S2 E0 R4[]") && im_all_ok());
    }
    verdict("im4", "command 3 answers c6_bulb, 0.5.0, ESP32-C6 and the board name (the default 'Zigbee Anthema <id>', or the stored one); command 4 answers one result per network (ssid, rssi as text, YES or NO, strongest first) then an empty result; 0 networks or a failed scan: the empty result only");

    /* im5. unknown command, unknown type, a session, the boot announcement, a failed driver */
    {   im_boot(); im_script(1000000, 1, 0, 0); im_run(30); im_cut(0);
        printf("im5 an IP arrives with no session begun: [%s]\n", im_desc()); CHECK(!strcmp(im_desc(), "S2") && im_all_ok());
        im_boot(); wf_got_ip(); im_script(0, 0, 0, 0); im_run(5); im_cut(0);
        printf("im5 booted with an IP: [%s]\n", im_desc()); CHECK(!strcmp(im_desc(), "S4") && im_all_ok());
        uint8_t t9[24]; size_t n9 = im_frame(t9, 1, 9, (const uint8_t[]){3, 0}, 2, 0);
        im_boot(); im_put(t9, n9, 200000); im_put_rpc(9, 500000); im_put_rpc(2, 3000000); im_script(2000000, 1, 4000000, 2); im_run(70); im_cut(0);
        printf("im5 type 9 at 0.2 s, command 9 at 0.5 s, an IP at 2 s, command 2 at 3 s, a disconnect at 4 s: [%s], checksums valid=%d, session=%d\n", im_desc(), im_all_ok(), s_improv_session);
        CHECK(!strcmp(im_desc(), "S2 E0 E2 S4 E0 S4 R2" IM_URLS " S2") && im_all_ok() && s_improv_session);
        im_boot(); g_usb_install_err = ESP_FAIL; size_t m = g_loglen; improv_start(); int t_fail = tasks_named("improv"), inst_fail = g_usb_installs, offs = log_count(m, "IMPROV off");
        printf("im5 driver install failing: installs=%d, improv task created=%d, 'IMPROV off' log lines=%d\n", inst_fail, t_fail, offs);
        CHECK(inst_fail == 1 && t_fail == 0 && offs == 1 && g_ctx_viol == 0);
        im_boot(); improv_start(); printf("im5 driver install ok: installs=%d, improv tasks=%d, stack %u\n", g_usb_installs, tasks_named("improv"), task_stack("improv"));
        CHECK(g_usb_installs == 1 && tasks_named("improv") == 1 && task_stack("improv") == 6144);
        CHECK(!im_leaks());
    }
    verdict("im5", "an unknown command is error 0 then error 2 and changes no state; an unknown frame type is ignored and starts no session; the state is sent once at boot and again at each change only after a session has begun (never before); command 2 mid-session answers state and URLs; a failed driver install logs once and creates no task");
}
