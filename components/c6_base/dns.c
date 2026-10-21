/*
 * dns — the hotspot's catch-all DNS.
 *
 * While the hotspot is up, a UDP server on 192.168.44.1:53 answers every A query with 192.168.44.1,
 * TTL 0, so a phone that joins the hotspot opens the setup page. Any other query type gets a reply
 * with no answer; a datagram that is not a standard one-question query gets no reply at all.
 *
 * One task, created by dns_start(). The c6_wifi_on_ap callback can run in the esp_timer task or the
 * button task, so it only records the wish and notifies; the socket is opened, served and closed by
 * the task alone. The receive timeout is what lets the task see a "down" while no query arrives.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "c6_wifi.h"

static const char *TAG = "dns";

#define DNS_PORT        53
#define DNS_BUF         512
#define DNS_HDR         12
#define DNS_ANSWER_LEN  16                   /* name pointer, type, class, TTL, length, address */
#define DNS_RECV_S      1                    /* how soon a "down" closes the socket when no query comes */
#define DNS_STACK       4096                 /* a log line costs ~2.5 KB here: ring_vprintf + vsnprintf */

static const uint8_t DNS_AP_IP[4] = {192, 168, 44, 1};

static TaskHandle_t s_dns_task;
static volatile bool s_dns_want;             /* the hotspot is up: serve */
static int s_dns_sock = -1;
static uint8_t s_dns_in[DNS_BUF];
static uint8_t s_dns_out[DNS_BUF];

/* The reply to one query of qlen bytes, into out. Its size; 0 for no reply: shorter than a header and
 * a question, a response, an opcode other than a standard query, not exactly one question, a name
 * that runs off the end or uses a pointer. An A query of class IN gets one answer, 192.168.44.1 with
 * TTL 0; any other type the question back with no answer. The id is echoed, QR and AA set, RD copied. */
static size_t dns_answer(const uint8_t *q, size_t qlen, uint8_t *out, size_t cap)
{
    if (qlen < DNS_HDR + 5 || qlen > DNS_BUF) {
        return 0;
    }
    if ((q[2] & 0x80) != 0 || (q[2] & 0x78) != 0) {
        return 0;
    }
    if (q[4] != 0 || q[5] != 1) {
        return 0;
    }
    size_t i = DNS_HDR;
    while (i < qlen && q[i] != 0) {
        if ((q[i] & 0xC0) != 0) {
            return 0;
        }
        i += (size_t)q[i] + 1;
    }
    if (i >= qlen) {
        return 0;
    }
    i++;                                     /* the root label */
    if (i + 4 > qlen) {
        return 0;
    }
    bool a = q[i] == 0 && q[i + 1] == 1 && q[i + 2] == 0 && q[i + 3] == 1;
    size_t qend = i + 4;
    size_t n = qend + (a ? DNS_ANSWER_LEN : 0);
    if (n > cap) {
        return 0;
    }
    memcpy(out, q, qend);
    out[2] = (uint8_t)(0x84 | (q[2] & 0x01));
    out[3] = 0;
    out[6] = 0;
    out[7] = a ? 1 : 0;
    memset(out + 8, 0, 4);
    if (a) {
        static const uint8_t rr[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
        memcpy(out + qend, rr, sizeof rr);
        memcpy(out + qend + sizeof rr, DNS_AP_IP, 4);
    }
    return n;
}

static bool dns_open(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGW(TAG, "DNS catch-all: no socket (errno %d)", errno);
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    memcpy(&addr.sin_addr.s_addr, DNS_AP_IP, 4);
    struct timeval tv = {.tv_sec = DNS_RECV_S, .tv_usec = 0};
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        ESP_LOGW(TAG, "DNS catch-all: bind or option failed (errno %d)", errno);
        close(s);
        return false;
    }
    s_dns_sock = s;
    ESP_LOGI(TAG, "DNS catch-all up on 192.168.44.1:53");
    return true;
}

static void dns_close(void)
{
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        ESP_LOGI(TAG, "DNS catch-all down");
    }
}

/* One receive: answer it if it deserves an answer. False on a socket error other than the timeout. */
static bool dns_serve_one(void)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(s_dns_sock, s_dns_in, sizeof s_dns_in, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        ESP_LOGW(TAG, "DNS catch-all: receive failed (errno %d); closed until the hotspot reopens", errno);
        return false;
    }
    size_t r = dns_answer(s_dns_in, (size_t)n, s_dns_out, sizeof s_dns_out);
    if (r > 0) {
        sendto(s_dns_sock, s_dns_out, r, 0, (struct sockaddr *)&from, fl);
    }
    return true;
}

static void dns_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_dns_want) {
            if (s_dns_sock < 0 && !dns_open()) {
                break;
            }
            if (!dns_serve_one()) {
                break;
            }
        }
        dns_close();
    }
}

/* The c6_wifi_on_ap callback: cheap, never blocks, no socket work. */
static void dns_on_ap(bool up)
{
    s_dns_want = up;
    if (s_dns_task != NULL) {
        xTaskNotifyGive(s_dns_task);
    }
}

void dns_start(void)
{
    if (s_dns_task == NULL && xTaskCreate(dns_task, "dns", DNS_STACK, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGW(TAG, "DNS catch-all off: task not created");
        return;
    }
    c6_wifi_on_ap(dns_on_ap);
    if (c6_wifi_ap_up()) {
        dns_on_ap(true);                     /* the hotspot opened before this callback was registered */
    }
}

void dns_stop(void)
{
    dns_on_ap(false);
}
