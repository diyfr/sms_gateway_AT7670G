/*
 * SPDX-FileCopyrightText: 2010-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "board.h"
#include "power.h"
#include "contacts.h"
#include "esp_modem_api.h"


static const int HTTPD_503_SERVICE_UNAVAILABLE = 503;

#define API_KEY_HEADER_NAME "x-api-key"

// Comparaison en temps constant pour limiter les attaques par mesure de timing
static bool api_key_matches(const char *provided)
{
    const char *expected = CONFIG_REST_API_KEY;
    size_t len_expected = strlen(expected);
    size_t len_provided = strlen(provided);
    if (len_expected != len_provided) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < len_expected; i++) {
        diff |= (unsigned char)(expected[i] ^ provided[i]);
    }
    return diff == 0;
}

// Vérifie l'en-tête X-API-Key sur chaque requête protégée ; envoie 401 et renvoie false si invalide
static bool authenticate_request(httpd_req_t *req)
{
    char provided_key[65] = {0};
    if (httpd_req_get_hdr_value_str(req, API_KEY_HEADER_NAME, provided_key, sizeof(provided_key)) != ESP_OK ||
        !api_key_matches(provided_key)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Clé API manquante ou invalide");
        return false;
    }
    return true;
}

static void add_cors_header(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// Parse une chaîne en entier positif strict (rejette les valeurs vides/non numériques)
static bool parse_positive_int(const char *str, int *out)
{
    if (str == NULL || str[0] == '\0') {
        return false;
    }
    char *endptr = NULL;
    long value = strtol(str, &endptr, 10);
    if (*endptr != '\0' || value < 0 || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

static void restart_after_delay_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void schedule_restart(void)
{
    xTaskCreate(restart_after_delay_task, "rest_restart", 2048, NULL, 5, NULL);
}

// Statut du modem/board : pas de protection, destiné à l'affichage public sur la page d'accueil
static esp_err_t board_status_get_handler(httpd_req_t *req) {
    add_cors_header(req);
    httpd_resp_set_type(req, "application/json");

    // Générer le JSON grâce à notre fonction board.c
    char *json_response = board_get_status_json(global_modem);

    if (json_response == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur d'allocation JSON");
        return ESP_FAIL;
    }

    // Envoyer la chaîne brute au client HTTP
    httpd_resp_sendstr(req, json_response);

    // ⚠️ ATTENTION : Libérer impérativement la mémoire allouée par cJSON
    free(json_response);
    
    return ESP_OK;
}



/**
 * 1. POST /api/v1/sms/send
 * Corps attendu (JSON) : {"to": "+33612345678", "text": "Votre message"}
 */
static esp_err_t sms_send_handler(httpd_req_t *req) {
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    char total_buf[512] = {0};
    
    if (global_modem == NULL) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "Modem déconnecté");
        return ESP_FAIL;
    }

    if (req->content_len >= sizeof(total_buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload trop lourd");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, total_buf, req->content_len);
    if (received <= 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(total_buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON Invalide");
        return ESP_FAIL;
    }

    cJSON *to = cJSON_GetObjectItem(root, "to");
    cJSON *text = cJSON_GetObjectItem(root, "text");

    if (!to || !text || !to->valuestring || !text->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Champs 'to' et 'text' requis");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_err_t err = board_send_sms(global_modem, to->valuestring, text->valuestring);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"SMS envoyé\"}");
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de l'envoi");
        return ESP_FAIL;
    }
}

/**
 * 2. GET /api/v1/sms/list
 * Retourne un tableau JSON des SMS stockés sur la SIM
 */
static esp_err_t sms_list_handler(httpd_req_t *req) {
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    if (global_modem == NULL) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "Modem déconnecté");
        return ESP_FAIL;
    }

    char *json_array = board_list_sms_json(global_modem);
    if (json_array == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur lors du listing");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_array);
    free(json_array);
    return ESP_OK;
}

/**
 * 3. GET /api/v1/sms/read?index=X
 * Extrait l'index depuis l'URL pour lire un message précis
 */
static esp_err_t sms_read_handler(httpd_req_t *req) {
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    if (global_modem == NULL) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "Modem déconnecté");
        return ESP_FAIL;
    }

    char query[64] = {0};
    char value[16] = {0};
    int index;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "index", value, sizeof(value)) != ESP_OK ||
        !parse_positive_int(value, &index)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Paramètre 'index' requis et doit être un entier positif");
        return ESP_FAIL;
    }

    char *json_str = board_read_sms_json(global_modem, index);

    if (json_str != NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de lecture");
        return ESP_FAIL;
    }
}

/**
 * 4. DELETE /api/v1/sms/delete?index=X
 * Extrait l'index depuis l'URL pour effacer un message précis
 */
static esp_err_t sms_delete_handler(httpd_req_t *req) {
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    if (global_modem == NULL) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "Modem déconnecté");
        return ESP_FAIL;
    }

    char query[64] = {0};
    char value[16] = {0};
    int index;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "index", value, sizeof(value)) != ESP_OK ||
        !parse_positive_int(value, &index)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Paramètre 'index' requis et doit être un entier positif");
        return ESP_FAIL;
    }

    esp_err_t err = board_delete_sms(global_modem, index);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"status\":\"success\",\"deleted_index\":%d}", index);
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de suppression");
        return ESP_FAIL;
    }
}



static const char *TAG = "esp-rest";

/**
 * GET /api/v1/contacts
 * Liste les contacts enregistrés (pas de protection, destiné à l'affichage public sur la page d'accueil)
 */
static esp_err_t contacts_list_get_handler(httpd_req_t *req)
{
    add_cors_header(req);

    char *json_array = contacts_list_json();
    if (json_array == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur d'allocation JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_array);
    free(json_array);
    return ESP_OK;
}

// Extrait les champs 'name'/'phone' du corps JSON de la requête ; renvoie false et une erreur HTTP si invalide
static bool parse_contact_body(httpd_req_t *req, char *name_out, size_t name_out_size, char *phone_out, size_t phone_out_size)
{
    char buf[128] = {0};
    if (req->content_len == 0 || req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload invalide");
        return false;
    }
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalide");
        return false;
    }

    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *phone = cJSON_GetObjectItem(root, "phone");
    if (!name || !phone || !name->valuestring || !phone->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Champs 'name' et 'phone' requis");
        cJSON_Delete(root);
        return false;
    }

    strlcpy(name_out, name->valuestring, name_out_size);
    strlcpy(phone_out, phone->valuestring, phone_out_size);
    cJSON_Delete(root);
    return true;
}

/**
 * POST /api/v1/contacts
 * Corps attendu (JSON) : {"name": "...", "phone": "+33..."}
 */
static esp_err_t contacts_create_post_handler(httpd_req_t *req)
{
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    char name[CONTACTS_NAME_MAX_LEN] = {0};
    char phone[CONTACTS_PHONE_MAX_LEN] = {0};
    if (!parse_contact_body(req, name, sizeof(name), phone, sizeof(phone))) {
        return ESP_FAIL;
    }

    int new_id = -1;
    esp_err_t err = contacts_add(name, phone, &new_id);
    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nombre maximum de contacts atteint (5)");
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de l'ajout du contact");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"success\",\"id\":%d}", new_id);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/**
 * PUT /api/v1/contacts?id=X
 * Corps attendu (JSON) : {"name": "...", "phone": "+33..."}
 */
static esp_err_t contacts_update_put_handler(httpd_req_t *req)
{
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    char query[64] = {0};
    char value[16] = {0};
    int id;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", value, sizeof(value)) != ESP_OK ||
        !parse_positive_int(value, &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Paramètre 'id' requis et doit être un entier positif");
        return ESP_FAIL;
    }

    char name[CONTACTS_NAME_MAX_LEN] = {0};
    char phone[CONTACTS_PHONE_MAX_LEN] = {0};
    if (!parse_contact_body(req, name, sizeof(name), phone, sizeof(phone))) {
        return ESP_FAIL;
    }

    esp_err_t err = contacts_update(id, name, phone);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Contact introuvable");
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de la mise à jour du contact");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

/**
 * DELETE /api/v1/contacts?id=X
 */
static esp_err_t contacts_delete_handler(httpd_req_t *req)
{
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    char query[64] = {0};
    char value[16] = {0};
    int id;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", value, sizeof(value)) != ESP_OK ||
        !parse_positive_int(value, &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Paramètre 'id' requis et doit être un entier positif");
        return ESP_FAIL;
    }

    esp_err_t err = contacts_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Contact introuvable");
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de la suppression du contact");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

/* Page d'accueil minimale embarquée dans le firmware */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>ESP A7670G Dashboard</title>"
    "<style>body{font-family:sans-serif;margin:2em;}h2{margin-top:1.5em;}"
    "table{border-collapse:collapse;}td{padding:4px 12px 4px 0;}td.k{font-weight:bold;}"
    "</style></head><body>"
    "<h1>ESP A7670G Dashboard</h1>"
    "<h2>Système</h2><table id='system'></table>"
    "<h2>Modem / Board</h2><table id='board'></table>"
    "<h2>Contacts</h2><table id='contacts'></table>"
    "<script>"
    "function fillTable(id, data) {"
    "  const t = document.getElementById(id);"
    "  t.innerHTML = '';"
    "  for (const key in data) {"
    "    const row = t.insertRow();"
    "    row.insertCell().outerHTML = '<td class=\"k\">' + key + '</td>';"
    "    row.insertCell().innerText = data[key];"
    "  }"
    "}"
    "function fillContacts(list) {"
    "  const t = document.getElementById('contacts');"
    "  t.innerHTML = '<tr><td class=\"k\">Id</td><td class=\"k\">Nom</td><td class=\"k\">Numéro</td></tr>';"
    "  for (const c of list) {"
    "    const row = t.insertRow();"
    "    row.insertCell().innerText = c.id;"
    "    row.insertCell().innerText = c.name;"
    "    row.insertCell().innerText = c.phone;"
    "  }"
    "}"
    "async function refresh() {"
    "  try {"
    "    const sys = await (await fetch('/api/v1/system/info')).json();"
    "    fillTable('system', sys);"
    "  } catch (e) { console.error(e); }"
    "  try {"
    "    const board = await (await fetch('/api/v1/board/status')).json();"
    "    fillTable('board', board);"
    "  } catch (e) { console.error(e); }"
    "  try {"
    "    const contacts = await (await fetch('/api/v1/contacts')).json();"
    "    fillContacts(contacts);"
    "  } catch (e) { console.error(e); }"
    "}"
    "refresh();"
    "setInterval(refresh, 5000);"
    "</script>"
    "</body></html>";

// Page d'accueil : pas de protection, informations non sensibles uniquement
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, INDEX_HTML);
}


/* Simple handler for getting system handler */
// Informations système : pas de protection, destiné à l'affichage public sur la page d'accueil
static esp_err_t system_info_get_handler(httpd_req_t *req)
{
    add_cors_header(req);

    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(root, "idf_version", IDF_VER);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    cJSON_AddStringToObject(root, "app_version", app_desc->version);
    cJSON_AddStringToObject(root, "project_name", app_desc->project_name);
    cJSON_AddNumberToObject(root, "battery_mv", power_get_battery_mv());
    cJSON_AddNumberToObject(root, "solar_mv", power_get_solar_mv());
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * POST /api/v1/system/reboot
 */
static esp_err_t system_reboot_handler(httpd_req_t *req)
{
    if (!authenticate_request(req)) {
        return ESP_FAIL;
    }
    add_cors_header(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Redémarrage en cours...\"}");
    schedule_restart();
    return ESP_OK;
}

esp_err_t start_rest_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Les commandes AT + parsing SMS empilent plusieurs buffers via des appels C++ profonds (esp_modem) :
    // la pile par défaut de 4096 octets ne suffit pas et provoque un stack overflow.
    config.stack_size = 8192;
    // Défaut de 8 handlers insuffisant maintenant que /api/v1/contacts enregistre 4 méthodes distinctes
    config.max_uri_handlers = 16;

    ESP_LOGI(TAG, "Starting HTTP Server");
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "Failed to start http server");

    /* Page d'accueil embarquée (statut système/board, sans protection) */
    httpd_uri_t index_get_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &index_get_uri);

    /* URI handler for fetching the board/modem status */
    httpd_uri_t board_status_get_uri = {
        .uri = "/api/v1/board/status",
        .method = HTTP_GET,
        .handler = board_status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &board_status_get_uri);


      /* 1. Route pour l'envoi de SMS (POST) */
    httpd_uri_t sms_send_uri = {
        .uri      = "/api/v1/sms/send",
        .method   = HTTP_POST,
        .handler  = sms_send_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sms_send_uri);

    /* 2. Route pour l'affichage de la liste (GET) */
    httpd_uri_t sms_list_uri = {
        .uri      = "/api/v1/sms/list",
        .method   = HTTP_GET,
        .handler  = sms_list_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sms_list_uri);

    /* 3. Route pour la lecture d'un SMS précis (GET avec paramètre ?index=X) */
    httpd_uri_t sms_read_uri = {
        .uri      = "/api/v1/sms/read",
        .method   = HTTP_GET,
        .handler  = sms_read_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sms_read_uri);

    /* 4. Route pour la suppression d'un SMS (DELETE avec paramètre ?index=X) */
    httpd_uri_t sms_delete_uri = {
        .uri      = "/api/v1/sms/delete",
        .method   = HTTP_DELETE,
        .handler  = sms_delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sms_delete_uri);

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/v1/system/info",
        .method = HTTP_GET,
        .handler = system_info_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* 5. Route pour redémarrer l'appareil (POST) */
    httpd_uri_t system_reboot_uri = {
        .uri      = "/api/v1/system/reboot",
        .method   = HTTP_POST,
        .handler  = system_reboot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_reboot_uri);

    /* Routes CRUD pour les contacts (liste publique, mutations protégées) */
    httpd_uri_t contacts_list_uri = {
        .uri      = "/api/v1/contacts",
        .method   = HTTP_GET,
        .handler  = contacts_list_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &contacts_list_uri);

    httpd_uri_t contacts_create_uri = {
        .uri      = "/api/v1/contacts",
        .method   = HTTP_POST,
        .handler  = contacts_create_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &contacts_create_uri);

    httpd_uri_t contacts_update_uri = {
        .uri      = "/api/v1/contacts",
        .method   = HTTP_PUT,
        .handler  = contacts_update_put_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &contacts_update_uri);

    httpd_uri_t contacts_delete_uri = {
        .uri      = "/api/v1/contacts",
        .method   = HTTP_DELETE,
        .handler  = contacts_delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &contacts_delete_uri);

    return ESP_OK;
}
