#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "wifi.h"        // for wifi_manager_set_sta_credentials()
#include "http_server.h" // our header

static const char *TAG = "HTTP_SERVER";

/**
 * @brief Minimal HTML form for Wi-Fi credentials
 */
static const char *HTML_FORM_PAGE =
"<!DOCTYPE html>"
"<html><head><title>Wi-Fi Setup</title></head>"
"<body>"
"<h2>Enter Wi-Fi Credentials</h2>"
"<form action=\"/set_creds\" method=\"post\">"
"  <label>SSID: <input type=\"text\" name=\"ssid\"></label><br><br>"
"  <label>Password: <input type=\"password\" name=\"pass\"></label><br><br>"
"  <input type=\"submit\" value=\"Connect\">"
"</form>"
"</body></html>";

/**
 * @brief URL-decode in-place (very simplified).
 *        Converts %XY to byte(HEX XY). Also treats '+' as space.
 */
static void url_decode(char *str)
{
    char *src = str, *dst = str;

    while (*src) {
        if (*src == '%') {
            // Expect two following hex digits
            if (src[1] && src[2]) {
                char hex[3];
                hex[0] = src[1];
                hex[1] = src[2];
                hex[2] = '\0';
                *dst++ = (char) strtol(hex, NULL, 16);
                src += 3;
            } else {
                // Malformed % at end, copy literally
                *dst++ = *src++;
            }
        } else if (*src == '+') {
            *dst++ = ' '; 
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Parse key=value pairs from `application/x-www-form-urlencoded`.
 *        E.g. "ssid=MyNetwork&pass=My%20Password"
 */
static esp_err_t parse_form_data(char *data, const char *key, char *out_val, size_t out_size)
{
    // Build pattern like "ssid="
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);

    // Locate pattern in data
    char *start = strstr(data, pattern);
    if (!start) {
        return ESP_ERR_NOT_FOUND; 
    }

    // Move past "key="
    start += strlen(pattern);

    // The value extends until '&' or end-of-string
    char *end = strchr(start, '&');
    if (!end) {
        end = start + strlen(start); // to the end
    }

    // Calculate length
    size_t val_len = end - start;
    if (val_len >= out_size) {
        val_len = out_size - 1;
    }

    memcpy(out_val, start, val_len);
    out_val[val_len] = '\0';

    // URL decode in place
    url_decode(out_val);

    return ESP_OK;
}

/**
 * @brief Handler for GET / : Serve the Wi-Fi form page
 */
static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_FORM_PAGE, strlen(HTML_FORM_PAGE));
    return ESP_OK;
}

/**
 * @brief Handler for POST /set_creds : parse SSID/PASS & call wifi_manager_set_sta_credentials
 */
static esp_err_t post_set_creds_handler(httpd_req_t *req)
{
    // 1) Read request content into a buffer
    const int max_form_size = 512;
    if (req->content_len <= 0 || req->content_len > max_form_size) {
        ESP_LOGW(TAG, "Invalid form size: %d", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form data too large or invalid");
        return ESP_FAIL;
    }

    char buf[max_form_size + 1];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received < 0) {
        ESP_LOGE(TAG, "Failed to receive form data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive form data");
        return ESP_FAIL;
    }
    buf[received] = '\0'; // Null-terminate

    ESP_LOGI(TAG, "Form data: %s", buf);

    // 2) Extract SSID and Password from the form data
    char ssid[32] = {0};
    char pass[64] = {0};

    esp_err_t got_ssid = parse_form_data(buf, "ssid", ssid, sizeof(ssid));
    esp_err_t got_pass = parse_form_data(buf, "pass", pass, sizeof(pass));

    if (got_ssid != ESP_OK || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "SSID not found or empty in form data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Received new creds: SSID='%s', PASS='%s'", ssid, pass);

    // 3) Immediately call wifi_manager_set_sta_credentials to save & connect
    esp_err_t err = wifi_manager_set_sta_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_set_sta_credentials failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set credentials");
        return ESP_FAIL;
    }

    // 4) Send a response page
    const char *resp_str = "<html><body><h2>Credentials saved. Trying to connect...</h2></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_str, strlen(resp_str));

    // 4) Delay briefly so the HTTP response can go out
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 5) Reboot
    esp_restart();
    
    return ESP_OK;
}

/**
 * @brief Handler for the Apple captive-portal checks (hotspot-detect.html, success.html).
 *        Return 200 OK with a short HTML that auto-redirects to "/".
 *
 *        This is more reliable for iOS/macOS because a 302 redirect can sometimes
 *        make iOS/macOS think the network is "broken" instead of "captive portal."
 */
static esp_err_t get_apple_hotspot_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    // A quick meta-refresh to "/":
    const char *redirect_page =
        "<html>"
        "<head><meta http-equiv=\"refresh\" content=\"0;url=/\"/></head>"
        "<body>Redirecting to captive portal...</body>"
        "</html>";
    return httpd_resp_send(req, redirect_page, strlen(redirect_page));
}

/**
 * @brief Handler for GET /generate_204 or /gen_204 for captive portal checks.
 *        If you want, you can still do a 302 redirect here for Android/Chrome style checks.
 */
static esp_err_t get_generate_204_handler(httpd_req_t *req)
{
    // Return a 302 redirect to "/"
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief Register URI handlers
 */
static void register_uris(httpd_handle_t server)
{
    // GET /
    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = get_root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);

    // POST /set_creds
    httpd_uri_t set_creds_uri = {
        .uri      = "/set_creds",
        .method   = HTTP_POST,
        .handler  = post_set_creds_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_creds_uri);

    // Apple captive-portal URIs:
    httpd_uri_t apple_hotspot_uri = {
        .uri      = "/hotspot-detect.html",
        .method   = HTTP_GET,
        .handler  = get_apple_hotspot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &apple_hotspot_uri);

    httpd_uri_t macos_success_uri = {
        .uri      = "/library/test/success.html",
        .method   = HTTP_GET,
        .handler  = get_apple_hotspot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &macos_success_uri);

    // For Android/Chrome-style captive portal checks:
    httpd_uri_t gen204_uri = {
        .uri      = "/generate_204",
        .method   = HTTP_GET,
        .handler  = get_generate_204_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &gen204_uri);

    httpd_uri_t short_uri = {
        .uri      = "/gen_204",
        .method   = HTTP_GET,
        .handler  = get_generate_204_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &short_uri);
}

/**
 * @brief Start the HTTP server
 */
void http_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;  // if you need more than the default

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        register_uris(server);
        ESP_LOGI(TAG, "HTTP server started.");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server.");
    }
}
