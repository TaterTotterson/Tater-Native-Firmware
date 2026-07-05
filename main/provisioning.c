#include "provisioning.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "tater_prov";
static tater_config_t s_initial;
static char s_ap_ssid[32];
static TaskHandle_t s_dns_task;
static esp_ip4_addr_t s_ap_ip;

static const char *HTML_HEAD =
    "<!doctype html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Tater Satellite Setup</title><style>"
    ":root{color-scheme:dark;--bg:#0b0f10;--panel:#141a1c;--panel2:#101517;--line:#263135;--line2:#38464b;--text:#f4f1e9;--muted:#a8b3ad;--orange:#ff8a2a;--orange2:#ffc07f;--green:#39d4a0;--red:#ff5757}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% -10%,rgba(255,138,42,.20),transparent 42%),linear-gradient(180deg,#111719 0%,#090c0d 100%);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif}"
    "main{max-width:760px;margin:0 auto;padding:24px 16px 34px}.shell{border:1px solid rgba(255,192,127,.18);background:linear-gradient(180deg,rgba(255,255,255,.055),rgba(255,255,255,.025)),rgba(14,18,20,.96);border-radius:18px;overflow:hidden;box-shadow:0 20px 70px rgba(0,0,0,.35)}"
    ".top{padding:24px 22px 18px;border-bottom:1px solid rgba(255,255,255,.08);background:linear-gradient(135deg,rgba(255,138,42,.12),rgba(57,212,160,.08))}.brand{display:flex;align-items:center;gap:12px;margin-bottom:14px}.mark{display:grid;place-items:center;width:42px;height:42px;border-radius:12px;background:linear-gradient(135deg,var(--orange),var(--orange2));color:#1a0d03;font-weight:900}.kicker{color:var(--orange2);font-size:12px;font-weight:800;letter-spacing:.08em;text-transform:uppercase}"
    "h1{font-size:28px;line-height:1.1;margin:0 0 8px}p{margin:0;color:var(--muted);line-height:1.45}.body{padding:22px}.status{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px;margin-bottom:20px}.chip{border:1px solid rgba(255,255,255,.08);background:rgba(0,0,0,.20);border-radius:12px;padding:11px 12px}.chip b{display:block;font-size:12px;color:var(--muted);font-weight:700}.chip span{display:block;margin-top:4px;color:var(--text);font-weight:750;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    "form{display:grid;gap:14px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}.field{display:grid;gap:7px}label{color:#dfe6e2;font-weight:750;font-size:13px}input{width:100%;border:1px solid var(--line2);border-radius:12px;background:#0f1416;color:var(--text);padding:13px 12px;font-size:16px;outline:none}input:focus{border-color:var(--orange2);box-shadow:0 0 0 3px rgba(255,138,42,.18)}.hint{color:var(--muted);font-size:12px;line-height:1.35}"
    "button{width:100%;border:0;border-radius:12px;background:linear-gradient(135deg,var(--orange),var(--orange2));color:#1d0e03;font-weight:900;padding:14px;font-size:16px;margin-top:4px}.foot{margin-top:16px;padding:14px;border:1px solid rgba(57,212,160,.18);background:rgba(57,212,160,.075);border-radius:12px;color:#c9f5e6;font-size:13px;line-height:1.4}.err{color:var(--red)}"
    "@media(max-width:640px){main{padding:10px}.shell{border-radius:0;border-left:0;border-right:0}.top,.body{padding:20px 16px}.grid{grid-template-columns:1fr}h1{font-size:25px}}"
    "</style></head><body><main><section class=shell>";

static void html_escape_attr(char *out, size_t out_len, const char *in)
{
    if (!out || out_len == 0) {
        return;
    }
    size_t pos = 0;
    const char *src = in ? in : "";
    while (*src && pos + 1 < out_len) {
        char c = *src++;
        const char *rep = NULL;
        if (c == '&') {
            rep = "&amp;";
        } else if (c == '"') {
            rep = "&quot;";
        } else if (c == '<') {
            rep = "&lt;";
        } else if (c == '>') {
            rep = "&gt;";
        }
        if (rep) {
            size_t rep_len = strlen(rep);
            if (pos + rep_len >= out_len) {
                break;
            }
            memcpy(out + pos, rep, rep_len);
            pos += rep_len;
        } else {
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
}

static void append_dns_name(uint8_t *response, int *offset, const uint8_t *query, int query_len, int *query_offset)
{
    while (*query_offset < query_len) {
        uint8_t len = query[*query_offset];
        response[(*offset)++] = len;
        (*query_offset)++;
        if (len == 0) {
            break;
        }
        if ((*query_offset + len) > query_len || (*offset + len) > 512) {
            break;
        }
        memcpy(response + *offset, query + *query_offset, len);
        *offset += len;
        *query_offset += len;
    }
}

static int build_dns_response(const uint8_t *query, int query_len, uint8_t *response, int response_len)
{
    if (!query || !response || query_len < 12 || response_len < 64) {
        return 0;
    }

    memset(response, 0, (size_t)response_len);
    response[0] = query[0];
    response[1] = query[1];
    response[2] = 0x81;
    response[3] = 0x80;
    response[4] = 0x00;
    response[5] = 0x01;
    response[6] = 0x00;
    response[7] = 0x01;

    int query_offset = 12;
    int offset = 12;
    append_dns_name(response, &offset, query, query_len, &query_offset);
    if (query_offset + 4 > query_len || offset + 20 > response_len) {
        return 0;
    }
    memcpy(response + offset, query + query_offset, 4);
    offset += 4;

    response[offset++] = 0xc0;
    response[offset++] = 0x0c;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x3c;
    response[offset++] = 0x00;
    response[offset++] = 0x04;
    uint32_t ip = s_ap_ip.addr;
    memcpy(response + offset, &ip, sizeof(ip));
    offset += 4;
    return offset;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "captive DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "captive DNS listening on 0.0.0.0:53");
    while (true) {
        uint8_t query[512];
        uint8_t response[512];
        struct sockaddr_in source_addr = {0};
        socklen_t source_len = sizeof(source_addr);
        int query_len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&source_addr, &source_len);
        if (query_len <= 0) {
            continue;
        }
        int response_len = build_dns_response(query, query_len, response, sizeof(response));
        if (response_len > 0) {
            sendto(sock, response, (size_t)response_len, 0, (struct sockaddr *)&source_addr, source_len);
        }
    }
}

static void start_dns_server(void)
{
    if (s_dns_task) {
        return;
    }
    BaseType_t ok = xTaskCreate(dns_task, "tater_dns", 4096, NULL, 5, &s_dns_task);
    if (ok != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGE(TAG, "captive DNS task create failed");
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char server[TATER_CFG_SERVER_URL_LEN * 2];
    char ssid[TATER_CFG_WIFI_SSID_LEN * 2];
    char name[TATER_CFG_DEVICE_NAME_LEN * 2];
    char room[TATER_CFG_ROOM_LEN * 2];
    html_escape_attr(server, sizeof(server), s_initial.server_url);
    html_escape_attr(ssid, sizeof(ssid), s_initial.wifi_ssid);
    html_escape_attr(name, sizeof(name), s_initial.device_name);
    html_escape_attr(room, sizeof(room), s_initial.room);

    char *page = calloc(1, 12000);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "GET / setup page");
    snprintf(
        page,
        12000,
        "%s"
        "<div class=top><div class=brand><div class=mark>T</div><div><div class=kicker>Tater Native</div><h1>Satellite Setup</h1></div></div>"
        "<p>Connect this Voice PE satellite to Wi-Fi and link it to your Tater server.</p></div>"
        "<div class=body>"
        "<div class=status><div class=chip><b>Setup Network</b><span>%s</span></div><div class=chip><b>Setup Address</b><span>192.168.4.1</span></div><div class=chip><b>Board</b><span>Voice PE</span></div></div>"
        "<form method=post action=/save>"
        "<div class=field><label for=ssid>Wi-Fi SSID</label><input id=ssid name=ssid value=\"%s\" autocomplete=off autocapitalize=none required><div class=hint>Your normal home Wi-Fi network.</div></div>"
        "<div class=field><label for=password>Wi-Fi Password</label><input id=password name=password type=password autocomplete=current-password><div class=hint>Leave blank only for open networks.</div></div>"
        "<div class=field><label for=server>Tater Server</label><input id=server name=server value=\"%s\" placeholder=\"http://10.4.20.210:8501\" autocapitalize=none required><div class=hint>Use the Tater app/server address this satellite should connect to.</div></div>"
        "<div class=grid><div class=field><label for=token>Pairing Code / Token</label><input id=token name=token value=\"\" autocapitalize=none autocomplete=one-time-code placeholder=\"123 456\" required><div class=hint>Open Tater Satellites, tap Add Satellite, then enter the code shown there.</div></div>"
        "<div class=field><label for=room>Room</label><input id=room name=room value=\"%s\" placeholder=\"Kitchen\"><div class=hint>Shown in Tater for routing and intercom.</div></div></div>"
        "<div class=field><label for=name>Device Name</label><input id=name name=name value=\"%s\" placeholder=\"Tater Voice PE\"><div class=hint>Friendly name shown in Tater.</div></div>"
        "<button type=submit>Save And Reboot</button>"
        "<div class=foot>After saving, this setup network will disappear. Reconnect your phone or computer to your normal Wi-Fi, then open Tater to confirm the satellite is connected.</div>"
        "</form></div></section></main></body></html>",
        HTML_HEAD,
        s_ap_ssid,
        ssid,
        server,
        room,
        name
    );
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            *w++ = (char)((hex_value(r[1]) << 4) | hex_value(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void set_field(tater_config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "ssid") == 0) {
        strlcpy(cfg->wifi_ssid, value, sizeof(cfg->wifi_ssid));
    } else if (strcmp(key, "password") == 0) {
        strlcpy(cfg->wifi_password, value, sizeof(cfg->wifi_password));
    } else if (strcmp(key, "server") == 0) {
        strlcpy(cfg->server_url, value, sizeof(cfg->server_url));
    } else if (strcmp(key, "token") == 0) {
        strlcpy(cfg->token, value, sizeof(cfg->token));
    } else if (strcmp(key, "name") == 0) {
        strlcpy(cfg->device_name, value, sizeof(cfg->device_name));
    } else if (strcmp(key, "room") == 0) {
        strlcpy(cfg->room, value, sizeof(cfg->room));
    }
}

static void parse_form(char *body, tater_config_t *cfg)
{
    char *saveptr = NULL;
    for (char *pair = strtok_r(body, "&", &saveptr); pair; pair = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = pair;
        char *value = eq + 1;
        url_decode(key);
        url_decode(value);
        set_field(cfg, key, value);
    }
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    int len = req->content_len;
    ESP_LOGI(TAG, "POST /save len=%d", len);
    if (len <= 0 || len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
        return ESP_OK;
    }

    char *body = calloc(1, (size_t)len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int read_total = 0;
    while (read_total < len) {
        int got = httpd_req_recv(req, body + read_total, len - read_total);
        if (got <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
            return ESP_OK;
        }
        read_total += got;
    }

    tater_config_t cfg = s_initial;
    parse_form(body, &cfg);
    free(body);

    if (!tater_config_has_wifi(&cfg) || strlen(cfg.server_url) == 0 || strlen(cfg.token) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi SSID, Tater server, and pairing code are required");
        return ESP_OK;
    }

    esp_err_t err = tater_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    char page[512];
    snprintf(
        page,
        sizeof(page),
        "%s<div class=top><div class=brand><div class=mark>T</div><div><div class=kicker>Tater Native</div><h1>Saved</h1></div></div>"
        "<p>Voice PE is rebooting and will connect to Tater.</p></div>"
        "<div class=body><div class=foot>Reconnect your phone or computer to your normal Wi-Fi, then open Tater to confirm this satellite is connected.</div></div>"
        "</section></main></body></html>",
        HTML_HEAD
    );
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "tater_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t redirect_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    ESP_LOGI(TAG, "redirecting setup request for %s", req->uri);
    return httpd_resp_send(req, "", 0);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start failed");

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_get_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "root handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "save handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &any), TAG, "catch-all handler failed");
    return ESP_OK;
}

const char *tater_provisioning_ssid(void)
{
    return s_ap_ssid;
}

esp_err_t tater_provisioning_start(const tater_config_t *initial)
{
    if (initial) {
        s_initial = *initial;
    } else {
        tater_config_defaults(&s_initial);
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Tater-Setup-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip_info = {0};
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        s_ap_ip = ip_info.ip;
    } else {
        s_ap_ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    }

    if (ap_netif) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = s_ap_ip.addr;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap_netif));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap_netif));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    start_dns_server();
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "http server failed");

    ESP_LOGW(TAG, "provisioning started: connect to %s; captive portal should open automatically or browse to http://192.168.4.1", s_ap_ssid);
    return ESP_OK;
}
