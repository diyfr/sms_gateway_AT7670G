#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "wifi_prov.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi_prov";

#define WIFI_PROV_NVS_NAMESPACE           "wifi_cfg"
#define WIFI_PROV_AP_SSID                 "ESP32-Setup"
#define WIFI_PROV_STA_CONNECT_TIMEOUT_MS  15000
#define WIFI_PROV_MAX_RETRY               5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static httpd_handle_t s_prov_httpd = NULL;

// ---------- Stockage des identifiants en NVS ----------

static esp_err_t wifi_prov_load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &password_size);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t wifi_prov_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// ---------- Connexion STA ----------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_PROV_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Reconnexion WiFi (%d/%d)...", s_retry_count, WIFI_PROV_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_prov_try_sta(const char *ssid, const char *password)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_count = 0;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = (strlen(password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connexion WiFi à '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_PROV_STA_CONNECT_TIMEOUT_MS));

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connecté au WiFi '%s'", ssid);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Échec de connexion au WiFi '%s'", ssid);
    ESP_ERROR_CHECK(esp_wifi_stop());
    return ESP_FAIL;
}

// ---------- Portail de configuration SoftAP ----------

static const char PROV_HTML_FORM[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>Configuration WiFi</title></head><body>"
    "<h1>Configuration WiFi du modem</h1>"
    "<form method='POST' action='/save'>"
    "SSID : <input name='ssid' maxlength='32' required><br><br>"
    "Mot de passe : <input name='password' type='password' maxlength='64'><br><br>"
    "<button type='submit'>Enregistrer et redémarrer</button>"
    "</form></body></html>";

static esp_err_t prov_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, PROV_HTML_FORM);
}

static void url_decode(char *dst, const char *src)
{
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void restart_after_delay_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static esp_err_t prov_save_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    size_t to_read = (req->content_len < sizeof(buf) - 1) ? req->content_len : sizeof(buf) - 1;
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Corps de requête invalide");
        return ESP_FAIL;
    }

    char raw_ssid[64] = {0};
    char raw_password[128] = {0};
    char ssid[33] = {0};
    char password[65] = {0};

    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK || strlen(raw_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Le champ 'ssid' est requis");
        return ESP_FAIL;
    }
    httpd_query_key_value(buf, "password", raw_password, sizeof(raw_password)); // optionnel (réseau ouvert)

    url_decode(ssid, raw_ssid);
    url_decode(password, raw_password);

    if (wifi_prov_save_credentials(ssid, password) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de l'enregistrement des identifiants");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h1>Identifiants enregistrés. Redémarrage...</h1></body></html>");

    xTaskCreate(restart_after_delay_task, "wifi_prov_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void wifi_prov_start_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_PROV_AP_SSID,
            .ssid_len = strlen(WIFI_PROV_AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Portail de configuration démarré : rejoignez '%s' puis ouvrez http://192.168.4.1/", WIFI_PROV_AP_SSID);

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_prov_httpd, &http_config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = prov_root_get_handler };
        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = prov_save_post_handler };
        httpd_register_uri_handler(s_prov_httpd, &root_uri);
        httpd_register_uri_handler(s_prov_httpd, &save_uri);
    }
}

esp_err_t wifi_prov_connect(void)
{
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, "esp32gw"); // Nom visible sur le réseau (DHCP), au lieu du défaut "espressif"

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    char ssid[33] = {0};
    char password[65] = {0};

    if (wifi_prov_load_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        if (wifi_prov_try_sta(ssid, password) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Les identifiants WiFi enregistrés ne fonctionnent plus, lancement du portail de configuration");
    } else {
        ESP_LOGI(TAG, "Aucun identifiant WiFi enregistré, lancement du portail de configuration");
    }

    wifi_prov_start_softap();
    // Le portail redémarre l'appareil une fois de nouveaux identifiants reçus ; on bloque ici indéfiniment.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_FAIL; // jamais atteint
}
