#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "driver/temperature_sensor.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "led_strip.h"
#include "nvs_flash.h"

static const char *TAG = "dashboard";

static int ws_broadcast(const char *msg);
static void led_set(uint8_t r, uint8_t g, uint8_t b);
static volatile int32_t led_manual = -1;
static volatile bool scanning;  // a second scan_start cancels the first, which then reports 0 APs  // -1 = status colours, else 0xRRGGBB picked in the dashboard

// Sends a scan result to every dashboard. Runs on the event task (never the httpd task,
// because ws_broadcast waits on httpd and would deadlock there).
static void send_scan_results(void)
{
    // Heap, not stack: 20 records are ~2 KB and the event task stack is small (4 KB, was 2.3 KB and crashed).
    uint16_t n = 20;
    wifi_ap_record_t *aps = calloc(n, sizeof(*aps));
    if (!aps || esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {  // also frees the driver's list
        esp_wifi_clear_ap_list();
        n = 0;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scan");
    cJSON *arr = cJSON_AddArrayToObject(root, "aps");
    for (int i = 0; i < n; i++) {
        cJSON *ap = cJSON_CreateObject();
        // ponytail: SSIDs with invalid UTF-8 would make the browser drop the socket; sanitize if it ever happens
        cJSON_AddStringToObject(ap, "ssid", (char *)aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "ch", aps[i].primary);
        cJSON_AddBoolToObject(ap, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, ap);
    }
    free(aps);
    char *msg = cJSON_PrintUnformatted(root);
    if (msg) {
        ws_broadcast(msg);
        cJSON_free(msg);
    }
    cJSON_Delete(root);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // ponytail: retries forever with no backoff; SoftAP fallback is Phase 6
        // reason 201 = SSID not found, 15/204 = wrong password (see esp_wifi_types.h)
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying", ev->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scanning = false;
        send_scan_results();
    }
}

static httpd_handle_t server;

static esp_err_t ws_reply(httpd_req_t *req, cJSON *obj)
{
    char *msg = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!msg) {
        return ESP_ERR_NO_MEM;
    }
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg)};
    esp_err_t err = httpd_ws_send_frame(req, &frame);
    cJSON_free(msg);
    return err;
}

static esp_err_t ws_error(httpd_req_t *req, const char *what)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "error");
    cJSON_AddStringToObject(r, "msg", what);
    return ws_reply(req, r);
}

// {"cmd":"ping","t":123}  -> {"type":"pong","t":123}  (client measures round trip)
// {"cmd":"scan"}          -> {"type":"scan","aps":[...]} to all clients when done
// {"cmd":"led","rgb":"#ff8800"} manual colour, {"cmd":"led","rgb":null} back to status
static esp_err_t handle_cmd(httpd_req_t *req, const char *text)
{
    cJSON *in = cJSON_Parse(text);
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(in, "cmd"));
    esp_err_t err = ESP_OK;

    if (!cmd) {
        err = ws_error(req, "bad json or missing cmd");
    } else if (!strcmp(cmd, "ping")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "pong");
        cJSON_AddItemToObject(r, "t", cJSON_Duplicate(cJSON_GetObjectItem(in, "t"), false) ?: cJSON_CreateNull());
        err = ws_reply(req, r);
    } else if (!strcmp(cmd, "scan")) {
        if (!scanning) {  // already running: its results go to every client anyway
            esp_err_t e = esp_wifi_scan_start(NULL, false);  // non-blocking; SCAN_DONE event sends results
            if (e == ESP_OK) {
                scanning = true;
            } else {
                err = ws_error(req, esp_err_to_name(e));
            }
        }
    } else if (!strcmp(cmd, "led")) {
        const char *rgb = cJSON_GetStringValue(cJSON_GetObjectItem(in, "rgb"));
        unsigned v;
        if (rgb && sscanf(rgb, "#%6x", &v) == 1) {
            led_manual = v;
            // ponytail: manual colours scaled to 1/4 brightness so #ffffff doesn't blind you
            led_set((v >> 16 & 0xff) / 4, (v >> 8 & 0xff) / 4, (v & 0xff) / 4);
        } else {
            led_manual = -1;  // main loop repaints status colour within 1 s
        }
    } else {
        err = ws_error(req, "unknown cmd");
    }
    cJSON_Delete(in);
    return err;
}

// Handshake arrives as GET; after that, every incoming frame lands here.
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected (fd %d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    uint8_t buf[128];
    httpd_ws_frame_t frame = {.payload = buf};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);  // len only
    if (err != ESP_OK || frame.len >= sizeof(buf)) {
        return ESP_FAIL;  // closes the socket
    }
    err = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    if (err != ESP_OK || frame.type != HTTPD_WS_TYPE_TEXT) {
        return err;
    }
    buf[frame.len] = 0;
    return handle_cmd(req, (char *)buf);
}

// Dashboard files, gzipped by `npm run build` and linked into the binary via EMBED_FILES.
extern const uint8_t index_html_gz[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz[] asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[] asm("_binary_app_css_gz_end");

typedef struct {
    const char *uri, *type;
    const uint8_t *start, *end;
} asset_t;

static const asset_t assets[] = {
    {"/", "text/html", index_html_gz, index_html_gz_end},
    {"/assets/app.js", "text/javascript", app_js_gz, app_js_gz_end},
    {"/assets/app.css", "text/css", app_css_gz, app_css_gz_end},
};

static esp_err_t asset_handler(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");  // names are fixed, so always revalidate
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;  // drop the oldest socket instead of refusing new clients
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_register_uri_handler(server, &ws);
    for (int i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
        httpd_uri_t u = {.uri = assets[i].uri, .method = HTTP_GET, .handler = asset_handler,
                         .user_ctx = (void *)&assets[i]};
        httpd_register_uri_handler(server, &u);
    }
}

static led_strip_handle_t led;
static SemaphoreHandle_t led_lock;  // LED is driven from both app_main and the httpd task

static void led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &led));
    led_lock = xSemaphoreCreateMutex();
}

// ponytail: brightness fixed at 20/255 (full is blinding); make it a Kconfig knob if needed
static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(led_lock, portMAX_DELAY);
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
    xSemaphoreGive(led_lock);
}

// Returns how many clients the message went to.
static int ws_broadcast(const char *msg)
{
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(server, &n, fds) != ESP_OK) {
        return 0;
    }
    int sent = 0;
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg)};
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            // runs on the httpd task, safe from here
            sent += httpd_ws_send_data(server, fds[i], &frame) == ESP_OK;
        }
    }
    return sent;
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();  // Wi-Fi driver stores calibration in NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    led_init();
    led_set(20, 0, 0);  // red until Wi-Fi is up
    wifi_init();
    http_start();

    temperature_sensor_handle_t tsens;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&tcfg, &tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(tsens));

    while (1) {
        float temp = 0;
        temperature_sensor_get_celsius(tsens, &temp);
        int rssi;
        char rssi_s[8] = "null";  // null while not associated
        bool online = esp_wifi_sta_get_rssi(&rssi) == ESP_OK;
        if (online) {
            snprintf(rssi_s, sizeof(rssi_s), "%d", rssi);
        }
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "{\"temp\":%.1f,\"heap\":%lu,\"heap_min\":%lu,\"heap_total\":%u,\"uptime\":%lld,\"rssi\":%s}",
                 temp, (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
                 esp_timer_get_time() / 1000000, rssi_s);
        // ponytail: LED state refreshes once per loop, so red/green can lag Wi-Fi by up to 1 s
        bool sent = ws_broadcast(msg) > 0;
        if (led_manual < 0) {  // otherwise the dashboard picked a colour; leave it alone
            if (sent) {
                led_set(0, 0, 20);  // blue flash: telemetry just went out
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            led_set(online ? 0 : 20, online ? 20 : 0, 0);  // green online, red offline
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
