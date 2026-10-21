/* ===== BOARD STUBS, Wi-Fi side (0.5.0): esp_wifi, esp_netif, the event loop, USB-Serial-JTAG, lwip sockets.
 * Included after board_stubs.c and before any firmware. Dependencies only. Passwords are kept for comparison, never printed. ===== */
#include <stddef.h>
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5006
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5005
#define ESP_ERR_ESP_NETIF_IF_NOT_READY 0x5003
#define ESP_EVENT_ANY_ID (-1)

/* ---- netif ---- */
struct esp_netif_obj {int id;};
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {esp_ip4_addr_t ip, netmask, gw;} esp_netif_ip_info_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)
static void esp_netif_set_ip4_addr(esp_ip4_addr_t *a, int b0, int b1, int b2, int b3) { a->addr = (uint32_t)b0 | (uint32_t)b1 << 8 | (uint32_t)b2 << 16 | (uint32_t)b3 << 24; }
static struct esp_netif_obj g_netif_sta = {1}, g_netif_ap = {2};
static int g_w_ap_netif_creates = 0, g_w_sta_netif_creates = 0, g_w_dhcps_stops = 0, g_w_dhcps_starts = 0, g_w_set_ip_infos = 0;
static esp_netif_ip_info_t g_w_ap_ip; static char g_w_hostname[32];
static esp_netif_t *esp_netif_create_default_wifi_sta(void) { CRIT_CALL(); ctx_check(); g_w_sta_netif_creates++; return &g_netif_sta; }
static esp_netif_t *esp_netif_create_default_wifi_ap(void) { CRIT_CALL(); ctx_check(); g_w_ap_netif_creates++; return &g_netif_ap; }
static int esp_netif_set_hostname(esp_netif_t *n, const char *h) { (void)n; snprintf(g_w_hostname, sizeof g_w_hostname, "%s", h); return 0; }
static int esp_netif_dhcps_stop(esp_netif_t *n) { (void)n; CRIT_CALL(); g_w_dhcps_stops++; return 0; }
static int esp_netif_dhcps_start(esp_netif_t *n) { (void)n; CRIT_CALL(); g_w_dhcps_starts++; return 0; }
static int esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *i) { (void)n; CRIT_CALL(); g_w_set_ip_infos++; g_w_ap_ip = *i; return 0; }

/* ---- the event loop: handlers kept, called directly by the probes ---- */
static const char g_wifi_event_base[] = "WIFI_EVENT", g_ip_event_base[] = "IP_EVENT";
#define WIFI_EVENT g_wifi_event_base
#define IP_EVENT g_ip_event_base
enum {WIFI_EVENT_STA_START = 2, WIFI_EVENT_STA_DISCONNECTED = 5, IP_EVENT_STA_GOT_IP = 0};
typedef struct {uint8_t reason;} wifi_event_sta_disconnected_t;
typedef struct {esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
static void (*g_ev_handlers[4])(void *, esp_event_base_t, int32_t, void *); static int g_ev_nhandlers = 0;
static int esp_event_handler_register(esp_event_base_t b, int32_t id, void (*h)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)b; (void)id; (void)arg; CRIT_CALL(); if (g_ev_nhandlers < 4) g_ev_handlers[g_ev_nhandlers++] = h; return 0;
}

/* ---- esp_wifi: every call recorded in order in g_wseq; the station's password kept only for comparison ---- */
typedef enum {WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA} wifi_mode_t;
typedef enum {WIFI_IF_STA, WIFI_IF_AP} wifi_interface_t;
typedef enum {WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK} wifi_auth_mode_t;
typedef enum {WIFI_PS_NONE, WIFI_PS_MIN_MODEM} wifi_ps_type_t;
typedef union {
    struct {uint8_t ssid[32]; uint8_t password[64];} sta;
    struct {uint8_t ssid[32]; uint8_t password[64]; uint8_t ssid_len; uint8_t channel; wifi_auth_mode_t authmode; uint8_t ssid_hidden; uint8_t max_connection;} ap;
} wifi_config_t;
typedef struct {uint8_t ssid[33]; int8_t rssi; uint8_t primary; wifi_auth_mode_t authmode;} wifi_ap_record_t;
typedef struct {int dummy;} wifi_scan_config_t;
typedef struct {int dummy;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
static char g_wseq[1024]; static int g_w_mode = WIFI_MODE_NULL, g_w_inits = 0, g_w_starts = 0, g_w_connects = 0, g_w_disconnects = 0, g_w_set_modes = 0;
static int g_w_modes[64], g_w_nmodes = 0, g_w_sta_sets = 0, g_w_ap_sets = 0, g_w_ps = -1;
static uint8_t g_w_sta_ssid[33]; static char g_w_sta_pass[65];            /* the last station config; the password is for comparison only */
static wifi_config_t g_w_ap_cfg;
static int g_w_connecting = 0, g_w_state_refusals = 0;            /* esp_wifi.h: set_config answers ESP_ERR_WIFI_STATE while the station is connecting */
static int g_w_set_config_err = 0, g_w_set_mode_err = 0, g_w_scan_err = 0, g_w_scan_calls = 0, g_w_sta_info_ok = 0, g_w_sta_channel = 0, g_w_clear_lists = 0;
static wifi_ap_record_t g_w_aps[48]; static int g_w_nap = 0; static void (*g_w_scan_hook)(void);
static void wseq(const char *s) { size_t n = strlen(g_wseq); if (n + strlen(s) + 2 < sizeof g_wseq) snprintf(g_wseq + n, sizeof g_wseq - n, "%s%s", n ? " " : "", s); }
static void wstub_reset(void) {
    g_wseq[0] = 0; g_w_mode = WIFI_MODE_NULL; g_w_inits = g_w_starts = g_w_connects = g_w_disconnects = g_w_set_modes = 0; g_w_nmodes = 0;
    g_w_sta_sets = g_w_ap_sets = 0; g_w_ps = -1; memset(g_w_sta_ssid, 0, sizeof g_w_sta_ssid); memset(g_w_sta_pass, 0, sizeof g_w_sta_pass); memset(&g_w_ap_cfg, 0, sizeof g_w_ap_cfg);
    g_w_connecting = g_w_state_refusals = 0;
    g_w_set_config_err = g_w_set_mode_err = g_w_scan_err = g_w_scan_calls = g_w_sta_info_ok = g_w_sta_channel = g_w_clear_lists = 0; g_w_nap = 0; g_w_scan_hook = NULL;
    g_w_ap_netif_creates = g_w_sta_netif_creates = g_w_dhcps_stops = g_w_dhcps_starts = g_w_set_ip_infos = 0; g_ev_nhandlers = 0; g_w_hostname[0] = 0;
}
static int esp_wifi_init(const wifi_init_config_t *c) { (void)c; CRIT_CALL(); wseq("init"); g_w_inits++; return 0; }
static int esp_wifi_start(void) { CRIT_CALL(); wseq("start"); g_w_starts++; return 0; }
static int esp_wifi_set_ps(wifi_ps_type_t t) { g_w_ps = (int)t; return 0; }
static int esp_wifi_set_mode(wifi_mode_t m) {
    CRIT_CALL(); wseq(m == WIFI_MODE_APSTA ? "mode:APSTA" : m == WIFI_MODE_STA ? "mode:STA" : "mode:other"); g_w_set_modes++;
    if (g_w_nmodes < 64) g_w_modes[g_w_nmodes++] = (int)m;
    if (g_w_set_mode_err) return g_w_set_mode_err;
    g_w_mode = (int)m; return 0;
}
static int esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c) {
    CRIT_CALL(); wseq(i == WIFI_IF_STA ? "set_config:STA" : "set_config:AP");
    if (g_w_set_config_err) return g_w_set_config_err;
    if (i == WIFI_IF_STA && g_w_connecting) { g_w_state_refusals++; return 0x3007; }
    if (i == WIFI_IF_STA) { g_w_sta_sets++; memcpy(g_w_sta_ssid, c->sta.ssid, 32); g_w_sta_ssid[32] = 0; memcpy(g_w_sta_pass, c->sta.password, 64); g_w_sta_pass[64] = 0; }
    else { g_w_ap_sets++; g_w_ap_cfg = *c; }
    return 0;
}
static int esp_wifi_connect(void) { CRIT_CALL(); wseq("connect"); g_w_connects++; g_w_connecting = 1; return 0; }
static int esp_wifi_disconnect(void) { CRIT_CALL(); wseq("disconnect"); g_w_disconnects++; g_w_connecting = 0; return 0; }
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *r) { if (!g_w_sta_info_ok) return ESP_FAIL; memset(r, 0, sizeof *r); r->primary = (uint8_t)g_w_sta_channel; r->rssi = -55; return 0; }
static int esp_wifi_scan_start(const wifi_scan_config_t *c, bool block) {
    (void)c; (void)block; CRIT_CALL(); wseq("scan"); g_w_scan_calls++;
    if (g_w_scan_hook) { void (*h)(void) = g_w_scan_hook; g_w_scan_hook = NULL; h(); }
    return g_w_scan_err;
}
static int esp_wifi_scan_get_ap_num(uint16_t *n) { *n = (uint16_t)g_w_nap; return 0; }
static int esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r) { if (*n > g_w_nap) *n = (uint16_t)g_w_nap; memcpy(r, g_w_aps, *n * sizeof *r); return 0; }
static int esp_wifi_clear_ap_list(void) { g_w_clear_lists++; return 0; }
/* the station's configured pair, as the last esp_wifi_set_config(STA) saw it (the probes compare; nothing prints the password) */
static bool w_sta_is(const char *ssid, const char *pass) { return !strcmp((const char *)g_w_sta_ssid, ssid) && !strcmp(g_w_sta_pass, pass); }
#define CONFIG_C6_AP_SSID_PREFIX "Zigbee-Anthema-"
#define CONFIG_C6_WIFI_FAIL_S 60
#define CONFIG_C6_WIFI_START_DELAY_MS 0
static char g_kconfig_wifi_ssid[40], g_kconfig_wifi_pass[70];
#define CONFIG_C6_WIFI_SSID g_kconfig_wifi_ssid
#define CONFIG_C6_WIFI_PASS g_kconfig_wifi_pass

/* ---- esp_netif_get_ip_info: the station's scripted address (improv's URLs) ---- */
static esp_netif_ip_info_t g_w_sta_ip_info;
static int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; CRIT_CALL(); *i = g_w_sta_ip_info; return 0; }

/* ---- USB-Serial-JTAG: a byte queue released by fake time; each read is counted and, when the quota runs out, leaves the task loop ---- */
typedef struct {uint32_t tx_buffer_size, rx_buffer_size;} usb_serial_jtag_driver_config_t;
static int g_usb_installs = 0, g_usb_install_err = 0, g_usb_reads_left = 0, g_usb_reads = 0;
static struct {uint8_t b; int64_t at;} g_usb_in[4096]; static int g_usb_in_n = 0, g_usb_in_pos = 0;
static uint8_t g_usb_out[65536]; static size_t g_usb_out_len = 0;           /* what this scenario wrote */
static uint8_t g_usb_all[262144]; static size_t g_usb_all_len = 0;          /* everything written in the run (cx3 scans it) */
static void (*g_usb_each)(int);                                             /* called at the start of each read with its number: a scripted event driver */
static void usb_reset(void) { g_usb_installs = g_usb_install_err = g_usb_reads_left = g_usb_reads = 0; g_usb_in_n = g_usb_in_pos = 0; g_usb_out_len = 0; g_usb_each = NULL;
    esp_netif_set_ip4_addr(&g_w_sta_ip_info.ip, 192, 0, 2, 77); }
static int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *c) { (void)c; g_usb_installs++; return g_usb_install_err; }
static int usb_serial_jtag_read_bytes(void *buf, uint32_t n, uint32_t ticks) {
    if (g_usb_reads_left <= 0) longjmp(loop_done, 1);
    g_usb_reads_left--; int rn = g_usb_reads++;
    if (g_usb_each) g_usb_each(rn);
    uint32_t k = 0;
    while (k < n && g_usb_in_pos < g_usb_in_n && g_usb_in[g_usb_in_pos].at <= fake_now) ((uint8_t *)buf)[k++] = g_usb_in[g_usb_in_pos++].b;
    if (k) return (int)k;
    fake_advance((int64_t)ticks * 1000); return 0;                          /* quiet: the read waits its whole timeout */
}
static int usb_serial_jtag_write_bytes(const void *buf, size_t n, uint32_t ticks) {
    (void)ticks;
    if (g_usb_out_len + n <= sizeof g_usb_out) { memcpy(g_usb_out + g_usb_out_len, buf, n); g_usb_out_len += n; }
    if (g_usb_all_len + n <= sizeof g_usb_all) { memcpy(g_usb_all + g_usb_all_len, buf, n); g_usb_all_len += n; }
    return (int)n;
}

/* ---- the setup page's side of the HTTP server: chunked HTML, 404, and the URI registry ---- */
typedef struct {const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); void *user_ctx;} httpd_uri_t;
static struct {char uri[48]; int method; esp_err_t (*handler)(httpd_req_t *);} g_reg[48]; static int g_nreg = 0, g_reg_err = 0;
static int httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u) {
    (void)h; CRIT_CALL(); if (g_reg_err) return g_reg_err;
    if (g_nreg < 48) { snprintf(g_reg[g_nreg].uri, sizeof g_reg[0].uri, "%s", u->uri); g_reg[g_nreg].method = u->method; g_reg[g_nreg].handler = u->handler; g_nreg++; }
    return 0;
}
static int httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s) {
    CRIT_CALL();
    if (!s) { r->code = r->status_code ? r->status_code : 200; return 0; }
    size_t have = strlen(r->response), n = strlen(s);
    if (have + n < sizeof r->response) { memcpy(r->response + have, s, n + 1); r->resp_len += (int)n; }
    resp_note(NULL, s, (long)n); return 0;
}
static int httpd_resp_send_404(httpd_req_t *r) { CRIT_CALL(); r->status_code = 404; r->code = 404; snprintf(r->response, sizeof r->response, "Nothing matches the given URI"); r->resp_len = 29; return 0; }

/* ---- lwip sockets: a datagram queue and what was sent back; the firmware's socket calls are renamed lw_* at extraction (dns.c) ---- */
#include <errno.h>
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
typedef unsigned socklen_t;
struct sockaddr {unsigned short sa_family; char sa_data[14];};
struct lw_in_addr {uint32_t s_addr;};
struct sockaddr_in {unsigned short sin_family; uint16_t sin_port; struct lw_in_addr sin_addr; char sin_zero[8];};
#define htons(x) ((uint16_t)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
static int g_lw_sockets = 0, g_lw_closes = 0, g_lw_binds = 0, g_lw_socket_err = 0, g_lw_bind_err = 0, g_lw_recv_err = 0, g_lw_rcvtimeo = -1, g_lw_empties = 0;
static uint16_t g_lw_port; static uint8_t g_lw_ip[4];
static struct {uint8_t b[700]; int n;} g_lw_q[48], g_lw_sent[48]; static int g_lw_qn = 0, g_lw_qpos = 0, g_lw_nsent = 0;
static void (*g_lw_empty)(void);                                           /* called each time a receive finds the queue empty (the receive timeout): the probe's "time passes" */
static void lw_reset(void) { g_lw_sockets = g_lw_closes = g_lw_binds = g_lw_socket_err = g_lw_bind_err = g_lw_recv_err = g_lw_empties = g_lw_qn = g_lw_qpos = g_lw_nsent = 0; g_lw_rcvtimeo = -1; g_lw_empty = NULL; g_nreg = g_reg_err = 0; }
static void lw_queue(const void *p, int n) { if (g_lw_qn < 48) { memcpy(g_lw_q[g_lw_qn].b, p, (size_t)n); g_lw_q[g_lw_qn++].n = n; } }
static int lw_socket(int d, int t, int p) { (void)d; (void)t; (void)p; CRIT_CALL(); ctx_check(); if (g_lw_socket_err) { errno = ENOMEM; return -1; } g_lw_sockets++; return 40 + g_lw_sockets; }
static int lw_bind(int s, const struct sockaddr *a, socklen_t l) {
    (void)s; (void)l; CRIT_CALL(); ctx_check(); const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    if (g_lw_bind_err) { errno = EADDRINUSE; return -1; }
    g_lw_binds++; g_lw_port = (uint16_t)(((in->sin_port & 0xff) << 8) | (in->sin_port >> 8)); memcpy(g_lw_ip, &in->sin_addr.s_addr, 4); return 0;
}
static int lw_setsockopt(int s, int lvl, int name, const void *v, socklen_t l) { (void)s; (void)lvl; (void)l; if (name == SO_RCVTIMEO) g_lw_rcvtimeo = (int)((const struct timeval *)v)->tv_sec; return 0; }
static int lw_recvfrom(int s, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *fl) {
    (void)s; (void)flags; (void)from; (void)fl; CRIT_CALL();
    if (g_lw_recv_err) { errno = ECONNRESET; return -1; }
    if (g_lw_qpos < g_lw_qn) { int len = g_lw_q[g_lw_qpos].n; if ((size_t)len > n) len = (int)n; memcpy(buf, g_lw_q[g_lw_qpos].b, (size_t)len); g_lw_qpos++; return len; }
    if (++g_lw_empties > 50) longjmp(loop_done, 1);                          /* a task that never leaves its loop */
    if (g_lw_empty) g_lw_empty();
    errno = EAGAIN; return -1;
}
static int lw_sendto(int s, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t tl) {
    (void)s; (void)flags; (void)to; (void)tl; CRIT_CALL();
    if (g_lw_nsent < 48 && n <= 700) { memcpy(g_lw_sent[g_lw_nsent].b, buf, n); g_lw_sent[g_lw_nsent++].n = (int)n; }
    return (int)n;
}
static int lw_close(int s) { (void)s; CRIT_CALL(); g_lw_closes++; return 0; }
