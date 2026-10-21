/*
 * portal — the Wi-Fi setup page and the captive-portal answers, on the one HTTP server.
 *
 *   GET  /setup   the form: the board's name, the stored SSID, a list of scanned networks with
 *                 "other…" last, a free ssid field and a password field that is never pre-filled.
 *                 Plain HTML: no JavaScript, nothing from another host. Every SSID and the name
 *                 are HTML-escaped.
 *   POST /setup   application/x-www-form-urlencoded, at most 256 bytes: ssid (or the picked
 *                 network), pass → c6_wifi_set. The answer names the SSID, never the password; a
 *                 refusal serves the form again with the reason. No auth, by decision: the page
 *                 changes the board's Wi-Fi and nothing else. Only while the hotspot is up
 *                 otherwise 403 with the form and the reason, the body unread.
 *   GET  /        302 to /setup, always.
 *   nine probe paths   302 to http://192.168.44.1/setup while the hotspot is up, else 404.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "c6_base.h"
#include "c6_wifi.h"

static const char *TAG = "portal";

#define PORTAL_BODY_MAX   256
#define PORTAL_NETS_MAX   16
#define PORTAL_SETUP_URL  "http://192.168.44.1/setup"

typedef struct {
    int n;
    char ssid[PORTAL_NETS_MAX][33];
} portal_nets_t;

/* ------------------------------------------------------------ text helpers */

/* A store the compiler cannot drop: for buffers that held the password. */
static void portal_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n > 0) {
        *v++ = 0;
        n--;
    }
}

/* in, HTML-escaped (& < > " '), into out; stops before an entity that would not fit. Its length. */
static size_t portal_escape(char *out, size_t cap, const char *in)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    for (; *in != 0; in++) {
        char one[2] = {*in, 0};
        const char *rep = one;
        switch (*in) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default:   break;
        }
        size_t l = strlen(rep);
        if (n + l >= cap) {
            break;
        }
        memcpy(out + n, rep, l);
        n += l;
    }
    out[n] = 0;
    return n;
}

static int portal_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* The bytes [s, end) form-decoded ('+' is a space, %XX a byte) into out. 1 when decoded; -1 for a
 * bad escape, an escaped NUL, or a value that needs more than cap - 1 bytes. */
static int portal_decode(const char *s, const char *end, char *out, size_t cap)
{
    if (cap == 0) {
        return -1;
    }
    size_t n = 0;
    while (s < end) {
        char c = *s++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (end - s < 2 || portal_hex(s[0]) < 0 || portal_hex(s[1]) < 0) {
                return -1;
            }
            c = (char)(portal_hex(s[0]) << 4 | portal_hex(s[1]));
            s += 2;
            if (c == 0) {
                return -1;
            }
        }
        if (n + 1 >= cap) {
            return -1;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

/* One field of a form body. 1: found and decoded into out; 0: absent; -1: malformed or too long. */
static int portal_form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (*p != 0) {
        const char *end = strchr(p, '&');
        if (end == NULL) {
            end = p + strlen(p);
        }
        if ((size_t)(end - p) > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            return portal_decode(p + kl + 1, end, out, cap);
        }
        p = (*end == '&') ? end + 1 : end;
    }
    return 0;
}

/* --------------------------------------------------------------- the pages */

static void portal_put(httpd_req_t *req, const char *s)
{
    httpd_resp_sendstr_chunk(req, s);
}

static void portal_put_esc(httpd_req_t *req, const char *s)
{
    char esc[400];
    portal_escape(esc, sizeof esc, s);
    httpd_resp_sendstr_chunk(req, esc);
}

static void portal_head(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    portal_put(req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    portal_put(req, title);
    portal_put(req, "</title></head><body>");
}

static esp_err_t portal_tail(httpd_req_t *req)
{
    portal_put(req, "<p>The LED shows a green double flash while this hotspot is up. Opened with the button, it "
                    "stays up for 10 minutes; opened because the board could not join, it closes once the board is on Wi-Fi.</p>"
                    "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* The form. current: the stored SSID or NULL; nets: the scan, or NULL when there is none to show;
 * reason: why the last submission was refused, or NULL. */
static esp_err_t portal_form(httpd_req_t *req, const char *name, const char *current,
                             const portal_nets_t *nets, const char *reason)
{
    portal_head(req, "Wi-Fi setup");
    portal_put(req, "<h1>");
    portal_put_esc(req, name);
    portal_put(req, "</h1>");
    if (current != NULL) {
        portal_put(req, "<p>Current network: <b>");
        portal_put_esc(req, current);
        portal_put(req, "</b></p>");
    } else {
        portal_put(req, "<p>No network is stored.</p>");
    }
    if (reason != NULL) {
        portal_put(req, "<p><b>Not saved: ");
        portal_put_esc(req, reason);
        portal_put(req, "</b></p>");
    }
    portal_put(req, "<form method=\"post\" action=\"/setup\">");
    if (nets != NULL) {
        portal_put(req, "<p><label>Network <select name=\"pick\">");
        for (int i = 0; i < nets->n && i < PORTAL_NETS_MAX; i++) {
            portal_put(req, "<option value=\"");
            portal_put_esc(req, nets->ssid[i]);
            portal_put(req, "\">");
            portal_put_esc(req, nets->ssid[i]);
            portal_put(req, "</option>");
        }
        portal_put(req, "<option value=\"\">other…</option></select></label></p>"
                        "<p><label>Other network <input name=\"ssid\" maxlength=\"32\"></label></p>");
    } else {
        portal_put(req, "<p><label>Network <input name=\"ssid\" maxlength=\"32\"></label></p>");
    }
    portal_put(req, "<p><label>Password <input name=\"pass\" type=\"password\" maxlength=\"63\"></label></p>"
                    "<p><button type=\"submit\">Save</button></p></form>");
    return portal_tail(req);
}

static void portal_scan_cb(const char *ssid, int rssi, bool secured, void *ctx)
{
    (void)rssi;
    (void)secured;
    portal_nets_t *nets = ctx;
    if (nets->n >= PORTAL_NETS_MAX || ssid[0] == 0) {
        return;
    }
    strlcpy(nets->ssid[nets->n], ssid, sizeof nets->ssid[0]);
    nets->n++;
}

/* The form with what the board knows now: its name, the stored SSID, a fresh scan. A scan that
 * cannot run (another is running: ESP_ERR_INVALID_STATE) leaves the list out. */
static esp_err_t portal_page(httpd_req_t *req, const char *reason)
{
    char name[64];
    char current[33];
    portal_nets_t nets;
    memset(&nets, 0, sizeof nets);
    c6_base_name(name, sizeof name);
    bool have = c6_wifi_ssid(current, sizeof current);
    esp_err_t err = c6_wifi_scan(portal_scan_cb, &nets);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SETUP page without the network list: %s", esp_err_to_name(err));
    }
    return portal_form(req, name, have ? current : NULL, err == ESP_OK ? &nets : NULL, reason);
}

static esp_err_t portal_saved(httpd_req_t *req, const char *ssid)
{
    portal_head(req, "Saved");
    portal_put(req, "<h1>Saved</h1><p>The board is connecting to <b>");
    portal_put_esc(req, ssid);
    portal_put(req, "</b>.</p>");
    return portal_tail(req);
}

/* ---------------------------------------------------------------- handlers */

static esp_err_t portal_setup_get(httpd_req_t *req)
{
    return portal_page(req, NULL);
}

/* The decision on a submitted body: NULL when c6_wifi_set took the settings (ssid_out holds the SSID),
 * else the reason to show. The text field wins over the picked network; an absent pass is empty. */
static const char *portal_apply(const char *body, char *ssid_out, size_t ssid_cap)
{
    char pass[65];
    const char *reason = NULL;
    int sr = portal_form_value(body, "ssid", ssid_out, ssid_cap);
    if (sr == 0 || (sr > 0 && ssid_out[0] == 0)) {
        sr = portal_form_value(body, "pick", ssid_out, ssid_cap);
    }
    int pr = portal_form_value(body, "pass", pass, sizeof pass);
    if (sr < 0) {
        reason = "the network name must be 1 to 32 characters.";
    } else if (sr == 0 || ssid_out[0] == 0) {
        reason = "enter or pick a network name.";
    } else if (pr < 0) {
        reason = "the password must be empty or 8 to 63 characters.";
    } else {
        esp_err_t err = c6_wifi_set(ssid_out, pr > 0 ? pass : "");
        if (err == ESP_ERR_INVALID_ARG) {
            reason = "the network name must be 1 to 32 characters and the password empty or 8 to 63, "
                     "with no control characters.";
        } else if (err != ESP_OK) {
            reason = "the board could not store the settings.";
        }
    }
    portal_wipe(pass, sizeof pass);
    return reason;
}

static esp_err_t portal_setup_post(httpd_req_t *req)
{
    char body[PORTAL_BODY_MAX + 1];
    if (!c6_wifi_ap_up()) {                       /* saving works only while the hotspot is open: nothing is read or stored */
        ESP_LOGW(TAG, "SETUP refused: the hotspot is not open");
        httpd_resp_set_status(req, "403 Forbidden");
        return portal_page(req, "Saving works only while the hotspot is open: hold the button 3–9 s.");
    }
    if (req->content_len > PORTAL_BODY_MAX) {
        return portal_page(req, "the form is longer than 256 bytes.");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            portal_wipe(body, sizeof body);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    body[got] = 0;
    char ssid[33];
    const char *reason = portal_apply(body, ssid, sizeof ssid);
    portal_wipe(body, sizeof body);
    if (reason != NULL) {
        ESP_LOGW(TAG, "SETUP refused: %s", reason);
        return portal_page(req, reason);
    }
    ESP_LOGI(TAG, "SETUP Wi-Fi settings stored; connecting to %s", ssid);
    return portal_saved(req, ssid);
}

static esp_err_t portal_redirect(httpd_req_t *req, const char *to)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", to);
    return httpd_resp_send(req, NULL, 0);
}

/* A phone's captive-portal check: sent to the setup page while the hotspot is up, else not found. */
static esp_err_t portal_probe(httpd_req_t *req)
{
    if (!c6_wifi_ap_up()) {
        return httpd_resp_send_404(req);
    }
    return portal_redirect(req, PORTAL_SETUP_URL);
}

static esp_err_t portal_root(httpd_req_t *req)
{
    return portal_redirect(req, "/setup");
}

void portal_register(httpd_handle_t h)
{
    const httpd_uri_t uris[] = {
        {.uri = "/setup",                     .method = HTTP_GET,  .handler = portal_setup_get},
        {.uri = "/setup",                     .method = HTTP_POST, .handler = portal_setup_post},
        {.uri = "/",                          .method = HTTP_GET,  .handler = portal_root},
        {.uri = "/generate_204",              .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/gen_204",                   .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/hotspot-detect.html",       .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/library/test/success.html", .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/connecttest.txt",           .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/ncsi.txt",                  .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/redirect",                  .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/canonical.html",            .method = HTTP_GET,  .handler = portal_probe},
        {.uri = "/success.txt",               .method = HTTP_GET,  .handler = portal_probe},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(h, &uris[i]));
    }
}
