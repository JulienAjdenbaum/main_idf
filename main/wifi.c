// wifi.c
#include "wifi.h"
#include "websocket_manager.h"  // optional if you want to start WebSocket upon connection
#include "dns_server.h"         // your DNS hijack
#include "http_server.h"        // your HTTP config page

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI_MGR";

// Maximum station connect retries
#define MAXIMUM_RETRY   5

// Access Point (Open) Wi-Fi config
#define AP_SSID         "MyESP32_OpenAP"
#define AP_CHANNEL      1
#define AP_MAX_CONN     4  // Max clients

// NVS keys where we store the credentials
#define NVS_NAMESPACE   "wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

// Keep a pointer to the AP netif (for logging IP, etc.)
static esp_netif_t* s_ap_netif = NULL;

// Internal station connection state
static int  s_retry_num       = 0;
static bool s_wifi_connected  = false;
static bool s_sta_init_done   = false;

// Our global event group
EventGroupHandle_t s_wifi_event_group = NULL;

// Forward declarations
static void wifi_init_sta(const char* ssid, const char* pass);
static void wifi_init_ap(void);
static void wifi_event_handler(void *arg, 
                               esp_event_base_t event_base,
                               int32_t event_id, 
                               void *event_data);
static esp_err_t load_wifi_creds_from_nvs(char* out_ssid, size_t ssid_size,
                                          char* out_pass, size_t pass_size);
static esp_err_t save_wifi_creds_to_nvs(const char* ssid, const char* pass);

/**
 * @brief Initialize Wi-Fi in station mode, using stored credentials if any.
 *        Fallback to AP if station fails or no credentials.
 */
esp_err_t wifi_manager_init(void)
{
    // Create the event group
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_FAIL;
    }

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || 
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Load credentials from NVS (if any)
    char ssid[32] = {0};
    char pass[64] = {0};
    bool has_creds = false;
    if (load_wifi_creds_from_nvs(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        if (strlen(ssid) > 0) {
            ESP_LOGI(TAG, "Loaded Wi-Fi creds from NVS: SSID=%s", ssid);
            has_creds = true;
        }
    }

    if (has_creds) {
        // Attempt station mode first with stored credentials
        wifi_init_sta(ssid, pass);
    } else {
        // Go straight to AP if no credentials stored
        ESP_LOGW(TAG, "No stored Wi-Fi creds. Starting AP...");
        wifi_init_ap();
    }

    return ESP_OK;
}

/**
 * @brief Initialize as a Wi-Fi station (STA).
 */
static void wifi_init_sta(const char* ssid, const char* pass)
{
    // Create default STA netif
    esp_netif_create_default_wifi_sta();

    // Initialize Wi-Fi if not done
    static bool wifi_inited = false;
    if (!wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        // Register Wi-Fi & IP event handlers
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, 
            &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, 
            &wifi_event_handler, NULL, NULL));

        wifi_inited = true;
    }

    // Set STA config
    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    // Use a threshold, or set it to WIFI_AUTH_OPEN if you want to allow all
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Start in STA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_sta_init_done = true;
    s_retry_num     = 0; // reset retry count
    ESP_LOGI(TAG, "STA init done. Trying SSID: %s", ssid);
}

/**
 * @brief Initialize as an open AP.
 */
static void wifi_init_ap(void)
{
    // Create default AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize Wi-Fi if not done
    static bool wifi_inited = false;
    if (!wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, 
            &wifi_event_handler, NULL, NULL));
        wifi_inited = true;
    }

    // Configure open AP
    wifi_config_t ap_config = { 0 };
    strncpy((char *)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len       = strlen(AP_SSID);
    ap_config.ap.channel        = AP_CHANNEL;
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.authmode       = WIFI_AUTH_OPEN; // no password

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP mode init done. SSID=%s (Open), Channel=%d", 
             AP_SSID, AP_CHANNEL);
}

/**
 * @brief Wi-Fi & IP event handler.
 */
static void wifi_event_handler(void *arg, 
                               esp_event_base_t event_base,
                               int32_t event_id, 
                               void *event_data)
{
    // Station started => connect
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    // Station disconnected event
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;

        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying STA... (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            ESP_LOGE(TAG, "Failed STA after %d retries; fallback to AP...", MAXIMUM_RETRY);

            // Stop STA
            esp_wifi_stop();

            // Only do this if not *already* AP
            // (or if you haven't created the netif yet)
            if (s_ap_netif == NULL) {
                // Start AP
                wifi_init_ap();
            }

            // Set a FAIL_BIT if your main loop wants to check it
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
}

    // Station got IP
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num      = 0;
        s_wifi_connected = true;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Signal success
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Optionally start WebSocket or other tasks here
        websocket_manager_init();
    }
    // AP started
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        if (s_ap_netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
                ESP_LOGI(TAG, "AP started. SSID=%s, IP=" IPSTR, AP_SSID, IP2STR(&ip_info.ip));
            }
        }
        // Start your HTTP server + DNS hijack for captive portal
        http_server_start();
        dns_server_start();
    }
    // AP station connected
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* ed = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected (AID=%d)",
                 MAC2STR(ed->mac), ed->aid);
    }
    // (Optional) AP station disconnected, etc., can handle here
}

/**
 * @brief Public function to set new STA credentials and re-init station mode.
 */
esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Storing new Wi-Fi creds: SSID=%s", ssid);

    // Save to NVS
    ESP_ERROR_CHECK(save_wifi_creds_to_nvs(ssid, pass));

    // Stop whatever mode we are currently in
    ESP_ERROR_CHECK(esp_wifi_stop());

    // Re-init station with new credentials
    wifi_init_sta(ssid, pass);

    return ESP_OK;
}

/**
 * @brief Helper to load credentials from NVS
 */
static esp_err_t load_wifi_creds_from_nvs(char* out_ssid, size_t ssid_size,
                                          char* out_pass, size_t pass_size)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Attempt to read SSID
    size_t required_size = ssid_size;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, out_ssid, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }

    // Attempt to read password
    required_size = pass_size;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASS, out_pass, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * @brief Helper to save credentials to NVS
 */
static esp_err_t save_wifi_creds_to_nvs(const char* ssid, const char* pass)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASS, pass);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}
