#include <stdio.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "string.h"
#include "cJSON.h"
#include "board.h"

[[maybe_unused]] static const char *TAG = "board";

esp_modem_dce_t* initialize_modem(void) {
    // esp_modem's DCE factory requires a non-null netif even when only AT/SMS commands are used
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *modem_netif = esp_netif_new(&netif_ppp_config);
    if (modem_netif == NULL) {
        ESP_LOGE(TAG, "Échec de la création du netif PPP du modem");
        return NULL;
    }

    // 1. Allumage électrique (Votre code identique)
    gpio_reset_pin(BOARD_MODEM_PWRKEY);
    gpio_set_direction(BOARD_MODEM_PWRKEY, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_MODEM_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); 
    gpio_set_level(BOARD_MODEM_PWRKEY, 0);
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    gpio_set_level(BOARD_MODEM_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(4000)); 

    // 2. Configuration matérielle de l'UART pour la lib (Plus besoin de uart_driver_install manuel !)
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = BOARD_MODEM_TX;
    dte_config.uart_config.rx_io_num = BOARD_MODEM_RX;
    dte_config.uart_config.event_queue_size = 30;
    dte_config.uart_config.rx_buffer_size = 2048;
    dte_config.uart_config.tx_buffer_size = 1024;

    // Aucun APN n'est requis pour les SMS : le contexte PDP n'est utilisé qu'en mode data (jamais activé ici)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(NULL); 
    // 3. Création de l'objet logiciel de la bibliothèque Espressif
    // Le A7670G se comporte comme un SIM7600 au niveau des commandes AT standards
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, modem_netif);
    
    if (dce == NULL) {
        ESP_LOGE(TAG, "Échec de l'initialisation logicielle du modem");
        esp_netif_destroy(modem_netif);
        return NULL;
    }

    return dce; // On retourne l'objet utilisable pour les SMS
}

/**
 * @brief 1. ENVOYER UN SMS
 * @param dce Le pointeur vers l'instance du modem initialisé
 */
// Translittère les caractères accentués UTF-8 courants (é, à, ç, ...) en ASCII : le modem envoie les
// SMS en jeu de caractères GSM/IRA et interprète mal les séquences UTF-8 multi-octets (ex: é -> "C)")
static void sms_utf8_to_ascii(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_size; ) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            out[o++] = (char)c;
            i++;
        } else if ((c & 0xE0) == 0xC0 && in[i + 1] != '\0') {
            unsigned int codepoint = ((c & 0x1F) << 6) | ((unsigned char)in[i + 1] & 0x3F);
            char replacement;
            switch (codepoint) {
                case 0xE0: case 0xE1: case 0xE2: case 0xE3: replacement = 'a'; break; // à á â ã
                case 0xC0: case 0xC1: case 0xC2: case 0xC3: replacement = 'A'; break;
                case 0xE8: case 0xE9: case 0xEA: case 0xEB: replacement = 'e'; break; // è é ê ë
                case 0xC8: case 0xC9: case 0xCA: case 0xCB: replacement = 'E'; break;
                case 0xEC: case 0xED: case 0xEE: case 0xEF: replacement = 'i'; break; // ì í î ï
                case 0xF2: case 0xF3: case 0xF4: case 0xF5: replacement = 'o'; break; // ò ó ô õ
                case 0xF9: case 0xFA: case 0xFB: case 0xFC: replacement = 'u'; break; // ù ú û ü
                case 0xE7: replacement = 'c'; break; // ç
                case 0xC7: replacement = 'C'; break;
                case 0xF1: replacement = 'n'; break; // ñ
                default:   replacement = '?'; break;
            }
            out[o++] = replacement;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3; // Séquence 3 octets (ex: guillemets typographiques, emoji BMP) : non supportée, ignorée
        } else if ((c & 0xF8) == 0xF0) {
            i += 4; // Séquence 4 octets (ex: emoji hors BMP) : non supportée, ignorée
        } else {
            i++; // Octet de continuation isolé/invalide
        }
    }
    out[o] = '\0';
}

esp_err_t board_send_sms(esp_modem_dce_t *dce, const char *phone_number, const char *message) {
    ESP_LOGI(TAG, "Envoi d'un SMS à %s...", phone_number);

    char ascii_message[256];
    sms_utf8_to_ascii(message, ascii_message, sizeof(ascii_message));

    // Configuration obligatoire : Mode Texte et Jeu de caractères GSM standard
    if (esp_modem_sms_txt_mode(dce, true) != ESP_OK || 
        esp_modem_sms_character_set(dce) != ESP_OK) {
        ESP_LOGE(TAG, "Échec de configuration du mode texte SMS");
        return ESP_FAIL;
    }

    // Utilisation de la fonction native de esp_modem
    esp_err_t err = esp_modem_send_sms(dce, phone_number, ascii_message);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SMS envoyé avec succès !");
    } else {
        ESP_LOGE(TAG, "Erreur lors de l'envoi du SMS: %d", err);
    }
    return err;
}

/**
 * @brief Interroge la capacité de stockage SMS de la SIM (AT+CPMS?)
 * @return Nombre total d'emplacements, ou -1 en cas d'échec
 */
static int get_sms_storage_capacity(esp_modem_dce_t *dce) {
    char response_buffer[2048] = {0};
    if (esp_modem_at_raw(dce, "AT+CPMS?\r\n", response_buffer, "OK", "ERROR", 2000) != ESP_OK) {
        return -1;
    }
    // La réponse ressemble à : +CPMS: "SM",2,10,"SM",2,10,"SM",2,10
    char *cpms_pos = strstr(response_buffer, "+CPMS:");
    int used = 0, total = 0;
    if (cpms_pos && sscanf(cpms_pos, "+CPMS: \"%*[^\"]\",%d,%d", &used, &total) == 2) {
        return total;
    }
    return -1;
}

// Avance *pos juste après la prochaine virgule (délimiteur de champ AT+CMGR/CMGL)
static void skip_to_next_field(const char **pos)
{
    const char *comma = strchr(*pos, ',');
    *pos = comma ? comma + 1 : *pos + strlen(*pos);
}

// Extrait un champ entre guillemets (gère les champs vides ""), avance *pos après le guillemet fermant
static void extract_quoted_field(const char **pos, char *out, size_t out_size)
{
    out[0] = '\0';
    if (**pos != '"') {
        return;
    }
    const char *start = *pos + 1;
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return;
    }
    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    *pos = end + 1;
}

// Retire les retours à la ligne/espaces en fin de chaîne
static void rstrip(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n' || text[len - 1] == ' ')) {
        text[--len] = '\0';
    }
}

// Retire le "OK" final de la réponse AT (uniquement présent après le dernier message)
static void strip_trailing_ok(char *text)
{
    rstrip(text);
    size_t len = strlen(text);
    if (len >= 2 && text[len - 2] == 'O' && text[len - 1] == 'K') {
        text[len - 2] = '\0';
        rstrip(text);
    }
}

// Construit un objet JSON à partir d'une réponse brute AT+CMGR ; NULL si l'emplacement est vide/invalide
static cJSON* parse_cmgr_response(const char *raw, int index)
{
    const char *marker = "+CMGR:";
    const char *rec = strstr(raw, marker);
    if (rec == NULL) {
        return NULL;
    }

    const char *header_start = rec + strlen(marker);
    while (*header_start == ' ') {
        header_start++;
    }
    const char *line_end = strchr(header_start, '\n');
    if (line_end == NULL) {
        return NULL;
    }

    char header_buf[128] = {0};
    size_t hlen = (size_t)(line_end - header_start);
    if (hlen >= sizeof(header_buf)) {
        hlen = sizeof(header_buf) - 1;
    }
    memcpy(header_buf, header_start, hlen);

    // Format AT+CMGR (mode texte) : +CMGR: "<status>","<sender>","<alpha>","<timestamp>"
    const char *p = header_buf;
    char status[24] = {0}, sender[32] = {0}, alpha[32] = {0}, timestamp[32] = {0};
    extract_quoted_field(&p, status, sizeof(status));
    skip_to_next_field(&p);
    extract_quoted_field(&p, sender, sizeof(sender));
    skip_to_next_field(&p);
    extract_quoted_field(&p, alpha, sizeof(alpha));
    skip_to_next_field(&p);
    extract_quoted_field(&p, timestamp, sizeof(timestamp));

    char text[2048] = {0};
    const char *text_start = line_end + 1;
    size_t tlen = strlen(text_start);
    if (tlen >= sizeof(text)) {
        tlen = sizeof(text) - 1;
    }
    memcpy(text, text_start, tlen);
    strip_trailing_ok(text);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "index", index);
    cJSON_AddStringToObject(msg, "status", status);
    cJSON_AddStringToObject(msg, "sender", sender);
    cJSON_AddStringToObject(msg, "timestamp", timestamp);
    cJSON_AddStringToObject(msg, "text", text);
    return msg;
}

/**
 * @brief Liste les SMS en interrogeant chaque emplacement individuellement (AT+CMGR), plutôt que de
 *        parser la réponse multi-lignes AT+CMGL (plus fragile face aux variations de format modem)
 * @return char* Tableau JSON alloué (à libérer avec free()), ou NULL en cas d'échec
 */
char* board_list_sms_json(esp_modem_dce_t *dce) {
    int capacity = get_sms_storage_capacity(dce);
    if (capacity <= 0) {
        capacity = 50; // Repli raisonnable si AT+CPMS? échoue
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }

    // Buffer alloué une seule fois sur le tas et réutilisé pour chaque emplacement interrogé
    char *raw = malloc(2048);
    if (raw == NULL) {
        cJSON_Delete(arr);
        return NULL;
    }

    for (int index = 1; index <= capacity; index++) {
        if (board_read_sms(dce, index, raw, 2048) != ESP_OK) {
            continue; // Emplacement vide ou invalide : on passe au suivant
        }
        cJSON *msg = parse_cmgr_response(raw, index);
        if (msg != NULL) {
            cJSON_AddItemToArray(arr, msg);
        }
    }

    free(raw);
    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json_str;
}

// Construit un tableau JSON à partir d'une réponse brute multi-messages AT+CMGL
static cJSON* parse_cmgl_response(const char *raw)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }

    const char *marker = "+CMGL:";
    const char *rec = strstr(raw, marker);
    while (rec != NULL) {
        const char *header_start = rec + strlen(marker);
        while (*header_start == ' ') {
            header_start++;
        }
        const char *line_end = strchr(header_start, '\n');
        if (line_end == NULL) {
            break;
        }

        char header_buf[128] = {0};
        size_t hlen = (size_t)(line_end - header_start);
        if (hlen >= sizeof(header_buf)) {
            hlen = sizeof(header_buf) - 1;
        }
        memcpy(header_buf, header_start, hlen);

        // Format AT+CMGL (mode texte) : +CMGL: <index>,"<status>","<sender>","<alpha>","<timestamp>"
        const char *p = header_buf;
        int index = atoi(p);
        char status[24] = {0}, sender[32] = {0}, alpha[32] = {0}, timestamp[32] = {0};
        skip_to_next_field(&p); // Passe l'index (non entre guillemets)
        extract_quoted_field(&p, status, sizeof(status));
        skip_to_next_field(&p);
        extract_quoted_field(&p, sender, sizeof(sender));
        skip_to_next_field(&p);
        extract_quoted_field(&p, alpha, sizeof(alpha));
        skip_to_next_field(&p);
        extract_quoted_field(&p, timestamp, sizeof(timestamp));

        // Le texte du message s'étend jusqu'au prochain "+CMGL:" ou à la fin de la réponse
        const char *text_start = line_end + 1;
        const char *next_rec = strstr(text_start, marker);
        size_t tlen = next_rec ? (size_t)(next_rec - text_start) : strlen(text_start);

        char text[2048] = {0};
        if (tlen >= sizeof(text)) {
            tlen = sizeof(text) - 1;
        }
        memcpy(text, text_start, tlen);
        strip_trailing_ok(text);

        cJSON *msg = cJSON_CreateObject();
        cJSON_AddNumberToObject(msg, "index", index);
        cJSON_AddStringToObject(msg, "status", status);
        cJSON_AddStringToObject(msg, "sender", sender);
        cJSON_AddStringToObject(msg, "timestamp", timestamp);
        cJSON_AddStringToObject(msg, "text", text);
        cJSON_AddItemToArray(arr, msg);

        rec = next_rec;
    }

    return arr;
}

/**
 * @brief Liste uniquement les SMS non lus (AT+CMGL="REC UNREAD"). Contrairement à AT+CMGR,
 *        AT+CMGL ne modifie pas le statut de lecture des messages : un SMS laissé de côté ici
 *        reste "non lu" et reste visible tel quel via /api/v1/sms/list ou l'application mobile.
 * @return char* Tableau JSON alloué (à libérer avec free()), ou NULL en cas d'échec
 */
char* board_list_unread_sms_json(esp_modem_dce_t *dce) {
    char *raw = malloc(4096); // Réponse potentiellement volumineuse si plusieurs SMS sont en attente
    if (raw == NULL) {
        return NULL;
    }
    memset(raw, 0, 4096);

    if (esp_modem_at_raw(dce, "AT+CMGL=\"REC UNREAD\"\r\n", raw, "OK", "ERROR", 5000) != ESP_OK) {
        free(raw);
        return NULL;
    }

    cJSON *arr = parse_cmgl_response(raw);
    free(raw);
    if (arr == NULL) {
        return NULL;
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json_str;
}

/**
 * @brief Marque un SMS comme lu sans exploiter son contenu (AT+CMGR change son statut en le lisant)
 */
esp_err_t board_mark_sms_read(esp_modem_dce_t *dce, int index) {
    return board_read_sms(dce, index, NULL, 0);
}

/**
 * @brief LIRE UN SMS SPÉCIFIQUE
 * @param index L'emplacement mémoire du SMS sur la SIM (ex: 1, 2, 3...)
 * @param out_buf Tampon recevant le texte brut de la réponse AT (peut être NULL)
 */
esp_err_t board_read_sms(esp_modem_dce_t *dce, int index, char *out_buf, size_t out_buf_size) {
    ESP_LOGI(TAG, "Lecture du SMS à l'index %d...", index);
    
    char cmd[32];
    char response_buffer[2048] = {0};
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d\r\n", index);

    esp_err_t err = esp_modem_at_raw(dce, cmd, response_buffer, "OK", "ERROR", 3000);
    
    if (err == ESP_OK) {
        printf("\n--- CONTENU DU SMS [%d] ---\n%s\n---------------------------\n", index, response_buffer);
        if (out_buf != NULL && out_buf_size > 0) {
            strlcpy(out_buf, response_buffer, out_buf_size);
        }
    } else {
        ESP_LOGE(TAG, "Erreur de lecture de l'index %d", index);
    }
    return err;
}

/**
 * @brief Lit un SMS et retourne un objet JSON structuré : {"index","status","sender","timestamp","text"}
 * @return char* Chaîne JSON allouée (à libérer avec free()), ou NULL en cas d'échec
 */
char* board_read_sms_json(esp_modem_dce_t *dce, int index) {
    char *raw = malloc(2048);
    if (raw == NULL) {
        return NULL;
    }
    if (board_read_sms(dce, index, raw, 2048) != ESP_OK) {
        free(raw);
        return NULL;
    }

    cJSON *msg = parse_cmgr_response(raw, index);
    free(raw);
    if (msg == NULL) {
        return NULL;
    }

    char *json_str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return json_str;
}

/**
 * @brief 4. SUPPRIMER UN SMS
 * @param index L'emplacement mémoire à vider sur la SIM
 */
esp_err_t board_delete_sms(esp_modem_dce_t *dce, int index) {
    ESP_LOGI(TAG, "Suppression du SMS à l'index %d...", index);
    
    char cmd[32];
    char response_buffer[2048] = {0};
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d\r\n", index);

    esp_err_t err = esp_modem_at_raw(dce, cmd, response_buffer, "OK", "ERROR", 3000);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SMS %d supprimé avec succès.", index);
    } else {
        ESP_LOGE(TAG, "Échec de suppression du SMS %d", index);
    }
    return err;
}

/**
 * @brief Supprime TOUS les SMS de la SIM (AT+CMGD=1,4 : delflag=4 = tous les messages, quel que soit leur statut)
 */
esp_err_t board_delete_all_sms(esp_modem_dce_t *dce) {
    ESP_LOGI(TAG, "Suppression de tous les SMS...");

    char response_buffer[2048] = {0};
    esp_err_t err = esp_modem_at_raw(dce, "AT+CMGD=1,4\r\n", response_buffer, "OK", "ERROR", 5000);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Tous les SMS ont été supprimés.");
    } else {
        ESP_LOGE(TAG, "Échec de la suppression de tous les SMS");
    }
    return err;
}


// Traduit le <stat> de AT+CNSMOD? en libellé lisible (2G/3G/4G)
static const char* network_type_label(int stat)
{
    switch (stat) {
        case 0: return "No Service";
        case 1: return "GSM (2G)";
        case 2: return "GPRS (2G)";
        case 3: return "EDGE (2G)";
        case 4: return "WCDMA (3G)";
        case 5: return "HSDPA (3G)";
        case 6: return "HSUPA (3G)";
        case 7: return "HSPA (3G)";
        case 8: return "LTE (4G)";
        default: return "unknown";
    }
}

/**
 * @brief Extrait la valeur suivant un label (ex: "Model:") jusqu'à la fin de ligne
 */
static void extract_field(const char *buf, const char *label, char *out, size_t out_size)
{
    out[0] = '\0';
    const char *pos = strstr(buf, label);
    if (pos == NULL) {
        return;
    }
    pos += strlen(label);
    while (*pos == ' ') {
        pos++;
    }
    size_t i = 0;
    while (*pos && *pos != '\r' && *pos != '\n' && i < out_size - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
}

/**
 * @brief Récupère l'état global du modem et génère une chaîne JSON clé-valeur
 * @param dce Le pointeur vers l'instance du modem
 * @return char* Un pointeur vers la chaîne JSON allouée en mémoire (PENSEZ À LA LIBÉRER avec free())
 */
char* board_get_status_json(esp_modem_dce_t *dce) {
    // 1. Initialiser l'objet racine JSON
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    // 2. Clé "is_up" : Vérifier si le modem répond à la commande de base AT
    // Buffer réutilisé pour tous les appels AT ci-dessous : doit faire au moins CONFIG_ESP_MODEM_C_API_STR_MAX
    char response_buffer[2048] = {0};
    esp_err_t err_at = esp_modem_at_raw(dce, "AT\r\n", response_buffer, "OK", "ERROR", 1000);
    cJSON_AddBoolToObject(root, "is_up", (err_at == ESP_OK));

    if (err_at == ESP_OK) {
        // 3. Clé "signal_rssi" : Utilisation de l'API native d'Espressif
        int rssi = 0, ber = 0;
        if (esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
            cJSON_AddNumberToObject(root, "signal_rssi", rssi);
        } else {
            cJSON_AddNumberToObject(root, "signal_rssi", -1);
        }

        // 4. Clé "modem_time" : Interroger l'horloge interne de la puce A7670G (AT+CCLK?)
        memset(response_buffer, 0, sizeof(response_buffer));
        if (esp_modem_at_raw(dce, "AT+CCLK?\r\n", response_buffer, "OK", "ERROR", 2000) == ESP_OK) {
            // La réponse ressemble à : +CCLK: "26/09/04,16:44:30+02"
            char *start = strchr(response_buffer, '"');
            if (start) {
                start++; // Avancer après le premier guillemet
                char *end = strchr(start, '"');
                if (end) *end = '\0'; // Couper au deuxième guillemet
                cJSON_AddStringToObject(root, "modem_time", start);
            } else {
                cJSON_AddStringToObject(root, "modem_time", "unknown_format");
            }
        } else {
            cJSON_AddStringToObject(root, "modem_time", "timeout");
        }

        // 5. Clé "sms_count" : Compter le nombre de SMS présents sur la carte SIM (AT+CPMS?)
        memset(response_buffer, 0, sizeof(response_buffer));
        if (esp_modem_at_raw(dce, "AT+CPMS?\r\n", response_buffer, "OK", "ERROR", 2000) == ESP_OK) {
            // La réponse ressemble à : +CPMS: "SM",2,10,"SM",2,10,"SM",2,10
            // Le premier chiffre après le premier guillemet est le nombre de SMS stockés
            // On cherche le marqueur dans tout le buffer : l'écho de commande du modem (ATE1)
            // peut précéder la réponse, ce qui ferait échouer un sscanf ancré au début.
            char *cpms_pos = strstr(response_buffer, "+CPMS:");
            int used = 0, total = 0;
            if (cpms_pos && sscanf(cpms_pos, "+CPMS: \"%*[^\"]\",%d,%d", &used, &total) >= 1) {
                cJSON_AddNumberToObject(root, "sms_count", used);
                cJSON_AddNumberToObject(root, "sms_max_capacity", total);
            } else {
                cJSON_AddNumberToObject(root, "sms_count", -1);
            }
        } else {
            cJSON_AddNumberToObject(root, "sms_count", -1);
        }

        // 6. Clé "smsc" : Numéro du centre de messagerie SMS (AT+CSCA?)
        memset(response_buffer, 0, sizeof(response_buffer));
        if (esp_modem_at_raw(dce, "AT+CSCA?\r\n", response_buffer, "OK", "ERROR", 2000) == ESP_OK) {
            // La réponse ressemble à : +CSCA: "+33609001390",145
            char *start = strchr(response_buffer, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) *end = '\0';
                cJSON_AddStringToObject(root, "smsc", start);
            } else {
                cJSON_AddStringToObject(root, "smsc", "unknown_format");
            }
        } else {
            cJSON_AddStringToObject(root, "smsc", "timeout");
        }

        // 7. Clé "network_registered" : État d'enregistrement sur le réseau (AT+CREG?)
        memset(response_buffer, 0, sizeof(response_buffer));
        if (esp_modem_at_raw(dce, "AT+CREG?\r\n", response_buffer, "OK", "ERROR", 2000) == ESP_OK) {
            // La réponse ressemble à : +CREG: 0,1 (stat: 1=domicile, 5=itinérance)
            // Même remarque que pour +CPMS: on cherche le marqueur au lieu d'ancrer le sscanf au début.
            char *creg_pos = strstr(response_buffer, "+CREG:");
            int mode = 0, stat = -1;
            if (creg_pos && sscanf(creg_pos, "+CREG: %d,%d", &mode, &stat) == 2) {
                cJSON_AddBoolToObject(root, "network_registered", (stat == 1 || stat == 5));
                cJSON_AddNumberToObject(root, "network_reg_status", stat);
            } else {
                cJSON_AddBoolToObject(root, "network_registered", false);
                cJSON_AddNumberToObject(root, "network_reg_status", -1);
            }
        } else {
            cJSON_AddBoolToObject(root, "network_registered", false);
            cJSON_AddNumberToObject(root, "network_reg_status", -1);
        }

        // 8. Identité du modem (ATI) : fabricant, modèle, révision, IMEI
        memset(response_buffer, 0, sizeof(response_buffer));
        if (esp_modem_at_raw(dce, "ATI\r\n", response_buffer, "OK", "ERROR", 2000) == ESP_OK) {
            char field[32] = {0};
            extract_field(response_buffer, "Manufacturer:", field, sizeof(field));
            cJSON_AddStringToObject(root, "manufacturer", field);
            extract_field(response_buffer, "Model:", field, sizeof(field));
            cJSON_AddStringToObject(root, "model", field);
            extract_field(response_buffer, "Revision:", field, sizeof(field));
            cJSON_AddStringToObject(root, "revision", field);
            extract_field(response_buffer, "IMEI:", field, sizeof(field));
            cJSON_AddStringToObject(root, "imei", field);
        }

        // 9. Clé "network_type" : Type de réseau data actif (AT+CNSMOD?)
        memset(response_buffer, 0, sizeof(response_buffer));
        if (esp_modem_at_raw(dce, "AT+CNSMOD?\r\n", response_buffer, "OK", "ERROR", 2000) == ESP_OK) {
            char *cnsmod_pos = strstr(response_buffer, "+CNSMOD:");
            int n = 0, stat = -1;
            if (cnsmod_pos && sscanf(cnsmod_pos, "+CNSMOD: %d,%d", &n, &stat) == 2) {
                cJSON_AddStringToObject(root, "network_type", network_type_label(stat));
            } else {
                cJSON_AddStringToObject(root, "network_type", "unknown");
            }
        } else {
            cJSON_AddStringToObject(root, "network_type", "unknown");
        }
    } else {
        // Si le modem est Down, on peuple avec des valeurs par défaut sécurisées
        cJSON_AddNumberToObject(root, "signal_rssi", -1);
        cJSON_AddStringToObject(root, "modem_time", "offline");
        cJSON_AddNumberToObject(root, "sms_count", -1);
        cJSON_AddStringToObject(root, "smsc", "offline");
        cJSON_AddBoolToObject(root, "network_registered", false);
        cJSON_AddNumberToObject(root, "network_reg_status", -1);
        cJSON_AddStringToObject(root, "network_type", "offline");
    }

    // 6. Convertir l'arborescence cJSON en une chaîne brute transmissible en HTTP
    char *json_string = cJSON_PrintUnformatted(root);
    
    // Libérer la mémoire intermédiaire utilisée par la structure cJSON
    cJSON_Delete(root);

    return json_string;
}


