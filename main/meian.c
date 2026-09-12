#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "cJSON.h"
#include "board.h"
#include "contacts.h"
#include "meian.h"

/**
https://github.com/wildstray/meian-client/blob/master/meian.py
* 
 */


static const char *TAG = "MEIAN_ALARM";

// Configurations réseau de votre centrale
#define ALARM_PORT       18034
#define RX_BUFFER_SIZE   2048
#define SOCK_TIMEOUT_MS  5000

// Table XOR réelle utilisée par le protocole Meian (identique à meian.py / pyialarm), 128 octets
static const char *XOR_KEY_HEX =
    "0c384e4e62382d620e384e4e44382d300f382b382b0c5a6234384e304e4c372b10535a0c20432d171142444e58422c421157322a204036172056446262382b5f"
    "0c384e4e62382d620e385858082e232c0f382b382b0c5a62343830304e2e362b10545a0c3e432e1711384e625824371c1157324220402c17204c444e624c2e12";
#define MEIAN_XOR_KEY_LEN 128
static uint8_t s_xor_key[MEIAN_XOR_KEY_LEN];
static bool s_xor_key_ready = false;

static void meian_xor_key_init(void)
{
    if (s_xor_key_ready) {
        return;
    }
    for (int i = 0; i < MEIAN_XOR_KEY_LEN; i++) {
        char byte_str[3] = { XOR_KEY_HEX[i * 2], XOR_KEY_HEX[i * 2 + 1], '\0' };
        s_xor_key[i] = (uint8_t)strtol(byte_str, NULL, 16);
    }
    s_xor_key_ready = true;
}

// Payloads XML des Commandes (Armer / Désarmer)
const char* XML_CMD_ARM_AWAY = "<Root><Host><SetAlarmStatus><DevStatus>TYP,ARM|0</DevStatus><Err></Err></SetAlarmStatus></Host></Root>";
const char* XML_CMD_ARM_STAY = "<Root><Host><SetAlarmStatus><DevStatus>TYP,STAY|0</DevStatus><Err></Err></SetAlarmStatus></Host></Root>";
const char* XML_CMD_DISARM   = "<Root><Host><SetAlarmStatus><DevStatus>TYP,DISARM|1</DevStatus><Err></Err></SetAlarmStatus></Host></Root>";
const char* XML_CMD_CLEAR   = "<Root><Host><SetAlarmStatus><DevStatus>TYP,CLEAR|1</DevStatus><Err></Err></SetAlarmStatus></Host></Root>";

// Payloads XML des Requêtes de Statuts
// NB : DevStatus/Err doivent être présents (même vides) comme le fait dicttoxml côté meian.py,
// sans quoi la centrale ignore la commande puis réinitialise la connexion après quelques secondes.
const char* XML_REQ_STATUS   = "<Root><Host><GetAlarmStatus><DevStatus></DevStatus><Err></Err></GetAlarmStatus></Host></Root>";


// Énumération des états possibles de la centrale
typedef enum {
    ALARM_STATE_DISARMED = 0,
    ALARM_STATE_ARMED_AWAY = 1,
    ALARM_STATE_ARMED_STAY = 2,
    ALARM_STATE_TRIGGERED   = 3, // Sirène en cours !
    ALARM_STATE_UNKNOWN     = -1
} meian_state_t;


// Dernier état connu (mis à jour par parse_alarm_status) et dernier état notifié par SMS aux contacts
static meian_state_t s_current_state = ALARM_STATE_UNKNOWN;
static meian_state_t s_last_notified_state = ALARM_STATE_UNKNOWN;

static const char* meian_state_label(meian_state_t alarm_state); // Défini plus bas

#define MEIAN_NVS_NAMESPACE "meian"
#define MEIAN_NVS_KEY       "config"
#define MEIAN_NVS_LAST_CID_KEY "last_cid"
#define MEIAN_LAST_CID_MAX_LEN 8

static meian_config_t s_config = {0};

// Dernier code Cid (évènement Push) connu, persisté en NVS pour survivre à un redémarrage
static char s_last_cid[MEIAN_LAST_CID_MAX_LEN] = {0};
static bool s_has_last_cid = false;

static const char* meian_cid_label(const char *cid); // Défini plus bas avec MEIAN_CID_TABLE

// Persiste le dernier code Cid reçu (ex: "1401") pour qu'il survive à un redémarrage
static void meian_persist_last_cid(const char *cid)
{
    strlcpy(s_last_cid, cid, sizeof(s_last_cid));
    s_has_last_cid = true;

    nvs_handle_t handle;
    if (nvs_open(MEIAN_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_str(handle, MEIAN_NVS_LAST_CID_KEY, s_last_cid);
    nvs_commit(handle);
    nvs_close(handle);
}

// Persiste l'intégralité de la configuration en un seul blob NVS
static esp_err_t meian_config_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MEIAN_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, MEIAN_NVS_KEY, &s_config, sizeof(s_config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t meian_config_init(void)
{
    memset(&s_config, 0, sizeof(s_config));

    nvs_handle_t handle;
    if (nvs_open(MEIAN_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK; // Aucune configuration enregistrée pour l'instant : valeurs par défaut
    }

    size_t size = sizeof(s_config);
    if (nvs_get_blob(handle, MEIAN_NVS_KEY, &s_config, &size) != ESP_OK) {
        // Rien de stocké, ou format incompatible avec une version précédente : on repart à vide
        memset(&s_config, 0, sizeof(s_config));
    }

    size_t cid_size = sizeof(s_last_cid);
    s_has_last_cid = (nvs_get_str(handle, MEIAN_NVS_LAST_CID_KEY, s_last_cid, &cid_size) == ESP_OK);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t meian_config_set(bool enabled, const char *ip, const char *user, const char *password)
{
    if (enabled && (ip == NULL || ip[0] == '\0' || user == NULL || user[0] == '\0' ||
                    password == NULL || password[0] == '\0')) {
        return ESP_ERR_INVALID_ARG; // IP, user et password sont obligatoires quand enabled == true
    }

    if (ip != NULL && ip[0] != '\0') {
        if (strlen(ip) >= MEIAN_IP_MAX_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        struct in_addr addr;
        if (inet_pton(AF_INET, ip, &addr) != 1) {
            return ESP_ERR_INVALID_ARG; // Adresse IPv4 invalide
        }
    }
    if (user != NULL && strlen(user) >= MEIAN_USER_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) >= MEIAN_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    meian_config_t previous = s_config;
    s_config.enabled = enabled;
    if (ip != NULL && ip[0] != '\0') {
        s_config.has_ip = true;
        strlcpy(s_config.ip, ip, sizeof(s_config.ip));
    } else {
        s_config.has_ip = false;
        s_config.ip[0] = '\0';
    }
    if (user != NULL && user[0] != '\0') {
        s_config.has_user = true;
        strlcpy(s_config.user, user, sizeof(s_config.user));
    } else {
        s_config.has_user = false;
        s_config.user[0] = '\0';
    }
    if (password != NULL && password[0] != '\0') {
        s_config.has_password = true;
        strlcpy(s_config.password, password, sizeof(s_config.password));
    } else {
        s_config.has_password = false;
        s_config.password[0] = '\0';
    }

    esp_err_t err = meian_config_save();
    if (err != ESP_OK) {
        s_config = previous; // Rollback en mémoire si la persistance échoue
        return err;
    }

    if (enabled && !previous.enabled) {
        meian_sync_alarm_state(); // Le canal Push ne fournit que les changements à venir, pas l'état initial
    }
    return err;
}

void meian_config_get(meian_config_t *out)
{
    *out = s_config;
}

char* meian_config_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "enabled", s_config.enabled);
    if (s_config.has_ip) {
        cJSON_AddStringToObject(root, "ip", s_config.ip);
    } else {
        cJSON_AddNullToObject(root, "ip");
    }
    if (s_config.has_user) {
        cJSON_AddStringToObject(root, "user", s_config.user);
    } else {
        cJSON_AddNullToObject(root, "user");
    }
    cJSON_AddBoolToObject(root, "has_password", s_config.has_password); // Le mot de passe n'est jamais exposé
    if (s_has_last_cid) {
        cJSON_AddStringToObject(root, "last_status_cid", s_last_cid);
        cJSON_AddStringToObject(root, "last_status_label", meian_cid_label(s_last_cid));
    } else {
        cJSON_AddNullToObject(root, "last_status_cid");
        cJSON_AddNullToObject(root, "last_status_label");
    }
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// Sérialise un statut public minimal (sans IP/identifiants), destiné à l'affichage sur la page
// d'accueil sans authentification : {"enabled":bool,"status_label":string|null}
char* meian_status_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "enabled", s_config.enabled);
    if (s_config.enabled && s_has_last_cid) {
        cJSON_AddStringToObject(root, "status_label", meian_cid_label(s_last_cid));
    } else {
        cJSON_AddNullToObject(root, "status_label");
    }
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}


/**
 * Arme l'alarme en mode Total (tous les capteurs actifs).
 */
bool arm(void) {
    ESP_LOGI(TAG, "⚡ Action : Armement Total demandé...");
    return execute_meian_transaction(XML_CMD_ARM_AWAY, false);
}


/**
 * Désarme le système et coupe la sirène si elle sonne.
 */
bool disarm(void) {
    ESP_LOGI(TAG, "⚡ Action : Désarmement demandé...");
    return execute_meian_transaction(XML_CMD_DISARM, false);
}


/**
 * Arme l'alarme en mode Périmétrique (uniquement portes et fenêtres).
 * Non utilisée
 */
bool perimeter(void) {
    ESP_LOGI(TAG, "⚡ Action : Armement Périmétrique demandé...");
    return execute_meian_transaction(XML_CMD_ARM_STAY, false);
}


/**
 * Interroge la centrale pour obtenir son état courant (commande SMS "CHECK").
 * @return meian_state_t L'état décodé de la centrale (ALARM_STATE_UNKNOWN si la requête échoue)
 */
bool state(void) {
    ESP_LOGI(TAG, "Commande CHECK : interrogation de la centrale...");
    return execute_meian_transaction(XML_REQ_STATUS, false);
}




/**
 * Applique le masque XOR Meian sur un buffer de données (Chiffrement / Déchiffrement)
 */
void meian_xor_cipher(char *data, int len) {
    meian_xor_key_init();
    for (int i = 0; i < len; i++) {
        data[i] = (char)((uint8_t)data[i] ^ s_xor_key[i & 0x7f]);
    }
}

/**
 * Analyse le XML de statut pour en extraire l'état global de la centrale
 */
void parse_alarm_status(const char *xml_response) {
    // Ordre des tests important : "DISARM" inclut le mot "ARM", on le teste donc en premier
    if (strstr(xml_response, "ALARM") != NULL) {
        ESP_LOGE(TAG, "🚨 STATUT : ALARME DÉCLENCHÉE EN COURS !");
        s_current_state = ALARM_STATE_TRIGGERED;
    } else if (strstr(xml_response, "DISARM") != NULL) {
        ESP_LOGI(TAG, "🔓 STATUT : Système désarmé.");
        s_current_state = ALARM_STATE_DISARMED;
    } else if (strstr(xml_response, "STAY") != NULL || strstr(xml_response, "HOME") != NULL) {
        ESP_LOGI(TAG, "🏠 STATUT : Armement Partiel / Périmétrique.");
        s_current_state = ALARM_STATE_ARMED_STAY;
    } else if (strstr(xml_response, "ARM") != NULL) {
        ESP_LOGI(TAG, "🔒 STATUT : Système armé.");
        s_current_state = ALARM_STATE_ARMED_AWAY;
    } else {
        ESP_LOGW(TAG, "❓ STATUT : État inconnu renvoyé par la centrale.");
        s_current_state = ALARM_STATE_UNKNOWN;
    }
}

/**
 * Analyse le XML de réponse des zones (<GetByWay>) pour détecter les anomalies
 */
void parse_zones_status(char *xml_response) {
    // On cible le conteneur de données qui contient la liste (ex: <DevStatus> ou <ByWayStatus>)
    char *start_tag = strstr(xml_response, "<DevStatus>");
    if (!start_tag) {
        start_tag = strstr(xml_response, "<ByWayStatus>");
    }

    if (start_tag) {
        char *data_start = strchr(start_tag, '>');
        if (data_start) {
            data_start++; // On avance juste après le '>'
            char *end_tag = strchr(data_start, '<');
            if (end_tag) {
                *end_tag = '\0'; // On isole temporairement la chaîne des zones (ex: "0,0,2,0,1")
                
                int zone_index = 1;
                char *token = strtok(data_start, ",");
                
                ESP_LOGI(TAG, "--- Rapport d'analyse des zones ---");
                while (token != NULL) {
                    int zone_state = atoi(token);
                    if (zone_state == 2) {
                        ESP_LOGE(TAG, " 🔥 ZONE %d : INTRUSION / DÉCLENCHEMENT SENSOR !", zone_index);
                    } else if (zone_state == 1) {
                        ESP_LOGW(TAG, " ⚠️ ZONE %d : Détecteur ouvert / Anomalie (Fenêtre ouverte ?)", zone_index);
                    }
                    zone_index++;
                    token = strtok(NULL, ",");
                }
                ESP_LOGI(TAG, "------------------------------------");
            }
        }
    } else {
        ESP_LOGW(TAG, "Impossible de localiser la balise des zones dans la réponse.");
    }
}

// Génère un token pseudo-aléatoire au format UUID (36 caractères), requis par la trame d'authentification
static void meian_generate_token(char *out, size_t out_size)
{
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) {
        rnd[i] = (uint8_t)(esp_random() & 0xFF);
    }
    snprintf(out, out_size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
             rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
}

// Construit et envoie une trame Meian complète (entête + XML chiffré + pied), au format "@ieM<len><seq>0000<xor><seq>"
static bool meian_send_frame(int sock, const char *xml_payload, int seq)
{
    int xml_len = (int)strlen(xml_payload);
    int tx_len = 16 + xml_len + 4;
    char *tx_buffer = malloc(tx_len + 1);
    if (tx_buffer == NULL) {
        ESP_LOGE(TAG, "Échec d'allocation mémoire pour l'envoi.");
        return false;
    }

    ESP_LOGI(TAG, "Meian TX (seq=%d) >> %s", seq, xml_payload);

    snprintf(tx_buffer, tx_len + 1, "@ieM%04d%04d0000", xml_len, seq);
    memcpy(tx_buffer + 16, xml_payload, xml_len);
    meian_xor_cipher(tx_buffer + 16, xml_len);
    snprintf(tx_buffer + 16 + xml_len, 5, "%04d", seq);

    bool sent_ok = send(sock, tx_buffer, tx_len, 0) == tx_len;
    if (!sent_ok) {
        ESP_LOGE(TAG, "Échec lors de l'envoi de la trame (errno %d).", errno);
    }
    free(tx_buffer);
    return sent_ok;
}

// Reçoit une trame Meian complète sur rx_buffer (taille RX_BUFFER_SIZE) et déchiffre le XML qu'elle contient
static bool meian_recv_frame(int sock, char *rx_buffer, char **xml_out, int *xml_len_out)
{
    memset(rx_buffer, 0, RX_BUFFER_SIZE);
    int total_received = 0;
    int expected_xml_len = -1;

    while (total_received < RX_BUFFER_SIZE - 1) {
        int bytes_received = recv(sock, rx_buffer + total_received, RX_BUFFER_SIZE - 1 - total_received, 0);

        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGE(TAG, "Timeout réseau atteint lors de la réception (%d octets reçus avant timeout).", total_received);
            } else {
                ESP_LOGE(TAG, "Erreur de réception socket: errno %d (%d octets reçus).", errno, total_received);
            }
            return false;
        } else if (bytes_received == 0) {
            break; // La centrale a fermé la connexion (fin du flux)
        }

        total_received += bytes_received;

        // Dès qu'on a reçu au moins l'entête (16 octets), on extrait la taille réelle attendue
        if (expected_xml_len == -1 && total_received >= 16) {
            if (strncmp(rx_buffer, "@ieM", 4) == 0) {
                char len_str[5] = {0};
                memcpy(len_str, rx_buffer + 4, 4);
                expected_xml_len = atoi(len_str);
            } else {
                ESP_LOGE(TAG, "Entête Meian invalide reçue (%d octets: %.16s).", total_received, rx_buffer);
                return false;
            }
        }

        // Si on a reçu l'entête (16) + le XML attendu + le pied (4), on a tout !
        if (expected_xml_len > 0 && total_received >= (16 + expected_xml_len + 4)) {
            break;
        }
    }

    if (expected_xml_len <= 0 || total_received < (16 + expected_xml_len + 4)) {
        ESP_LOGW(TAG, "Paquet incomplet ou corrompu. Reçu: %d octets.", total_received);
        return false;
    }

    char *xml_ptr = rx_buffer + 16;
    meian_xor_cipher(xml_ptr, expected_xml_len);
    xml_ptr[expected_xml_len] = '\0'; // Clôture propre de la chaîne C

    ESP_LOGI(TAG, "Meian RX << %s", xml_ptr);

    *xml_out = xml_ptr;
    *xml_len_out = expected_xml_len;
    return true;
}

// La réponse porte un code <Err>ERR|NN</Err> en tête (avant même <Root>) : NN == 00 signifie succès/identifiants valides
static bool meian_response_ok(const char *xml_response)
{
    const char *marker = "<Err>ERR|";
    const char *err = strstr(xml_response, marker);
    if (err == NULL) {
        return true; // Pas de code d'erreur explicite : réponse considérée valide
    }
    int code = atoi(err + strlen(marker));
    return code == 0;
}

// Authentifie la session (handshake <Pair><Client>) avant toute commande, comme l'exige le protocole Meian
static bool meian_authenticate(int sock, char *rx_buffer)
{
    char token[40];
    meian_generate_token(token, sizeof(token));

    char login_xml[320];
    snprintf(login_xml, sizeof(login_xml),
             "<Root><Pair><Client><Id>STR,%d|%s</Id><Pwd>PWD,%d|%s</Pwd><Type>TYP,ANDROID|0</Type>"
             "<Token>STR,%d|%s</Token><Action>TYP,IN|0</Action><Err></Err></Client></Pair></Root>",
             (int)strlen(s_config.user), s_config.user,
             (int)strlen(s_config.password), s_config.password,
             (int)strlen(token), token);

    if (!meian_send_frame(sock, login_xml, 1)) {
        return false;
    }

    char *xml_response = NULL;
    int xml_len = 0;
    if (!meian_recv_frame(sock, rx_buffer, &xml_response, &xml_len)) {
        return false;
    }

    if (!meian_response_ok(xml_response)) {
        ESP_LOGE(TAG, "Authentification refusée par la centrale Meian (identifiants invalides ?).");
        return false;
    }
    return true;
}

/**
 * Envoie une commande XML à la centrale : s'authentifie (Pair/Client) puis envoie la commande sur la même session
 */
bool execute_meian_transaction(const char *xml_payload, bool is_zone_query) {
    if (!s_config.has_ip || !s_config.has_user || !s_config.has_password) {
        ESP_LOGE(TAG, "Configuration Meian incomplète (IP/identifiants) : transaction annulée.");
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Impossible de créer la socket TCP: errno %d", errno);
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec = SOCK_TIMEOUT_MS / 1000;
    timeout.tv_usec = (SOCK_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(s_config.ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(ALARM_PORT);

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Échec de connexion à la centrale Meian (%s).", s_config.ip);
        close(sock);
        return false;
    }

    char *rx_buffer = malloc(RX_BUFFER_SIZE);
    if (rx_buffer == NULL) {
        ESP_LOGE(TAG, "Échec d'allocation mémoire pour la réception.");
        close(sock);
        return false;
    }
    bool success = false;
    if (meian_authenticate(sock, rx_buffer)) {
        char *xml_response = NULL;
        int xml_len = 0;
        if (meian_send_frame(sock, xml_payload, 2) &&
            meian_recv_frame(sock, rx_buffer, &xml_response, &xml_len)) {
            success = meian_response_ok(xml_response);
            if (!success) {
                ESP_LOGW(TAG, "La centrale a renvoyé un code d'erreur pour cette commande.");
            }
            if (is_zone_query) {
                parse_zones_status(xml_response);
            } else {
                parse_alarm_status(xml_response);
            }
        }
    }

    free(rx_buffer);
    close(sock);
    return success;
}


/**
 * Convertit un état d'alarme en libellé lisible, utilisé dans les SMS de notification
 */
static const char* meian_state_label(meian_state_t alarm_state)
{
    switch (alarm_state) {
        case ALARM_STATE_DISARMED:   return "désarmée";
        case ALARM_STATE_ARMED_AWAY: return "armée (totale)";
        case ALARM_STATE_ARMED_STAY: return "armée (périmétrique)";
        case ALARM_STATE_TRIGGERED:  return "déclenchée";
        default:                     return "état inconnu";
    }
}

// Envoie un message à tous les contacts enregistrés, en dédupliquant par numéro de téléphone (au cas
// où un même contact serait enregistré plusieurs fois) pour éviter d'envoyer le même SMS en double
static void meian_broadcast_sms(const char *message)
{
    contact_info_t contacts[CONTACTS_MAX_COUNT];
    int count = contacts_get_all(contacts, CONTACTS_MAX_COUNT);

    char sent_phones[CONTACTS_MAX_COUNT][CONTACTS_PHONE_MAX_LEN];
    int sent_count = 0;

    for (int i = 0; i < count; i++) {
        bool already_sent = false;
        for (int j = 0; j < sent_count; j++) {
            if (strcmp(sent_phones[j], contacts[i].phone) == 0) {
                already_sent = true;
                break;
            }
        }
        if (already_sent) {
            continue;
        }
        board_send_sms(global_modem, contacts[i].phone, message);
        if (sent_count < CONTACTS_MAX_COUNT) {
            strlcpy(sent_phones[sent_count], contacts[i].phone, sizeof(sent_phones[sent_count]));
            sent_count++;
        }
    }
}

// Envoie un SMS de notification à tous les contacts enregistrés (le PIN ne sert qu'à autoriser les commandes SMS)
static void meian_notify_contacts_state_change(meian_state_t new_state)
{
    char message[64];
    snprintf(message, sizeof(message), "Alarme : %s", meian_state_label(new_state));
    meian_broadcast_sms(message);
}

// --------------------------------------------------------------------------------------------
// Canal Push (<Pair><Push>) : notifie en temps réel les évènements (Cid), cf. MeianPushClient
// dans meian.py. Contrairement aux commandes classiques, l'inscription Push ne nécessite pas
// d'authentification préalable (<Pair><Client>) : seul l'identifiant (Id) est envoyé.
// --------------------------------------------------------------------------------------------

#define MEIAN_PUSH_KEEPALIVE_MS  60000 // Fréquence d'envoi du ping "%maI" pour maintenir la session
#define MEIAN_PUSH_RECONNECT_MS  10000 // Délai avant nouvelle tentative après une déconnexion/échec
#define MEIAN_PUSH_RECV_TIMEOUT_S 5     // Timeout court pour pouvoir réémettre le keepalive à temps

// Table des codes évènements (Cid) renvoyés par la centrale, cf. dictionnaire Cid de meian.py
typedef struct {
    const char *cid;
    const char *label;
} meian_cid_entry_t;

static const meian_cid_entry_t MEIAN_CID_TABLE[] = {
    { "1100", "Alarme médicale" },
    { "1101", "Urgence" },
    { "1110", "Incendie" },
    { "1120", "Urgence" },
    { "1131", "Intrusion périmétrique" },
    { "1132", "Intrusion / Cambriolage" },
    { "1133", "Alarme 24h" },
    { "1134", "Alarme temporisée" },
    { "1137", "Détecteur démonté" },
    { "1301", "Coupure secteur" },
    { "1302", "Batterie centrale faible" },
    { "1306", "Modification de programmation" },
    { "1350", "Panne de communication" },
    { "1351", "Ligne téléphonique en défaut" },
    { "1370", "Défaut de boucle" },
    { "1381", "Détecteur perdu" },
    { "1384", "Détecteur en batterie faible" },
    { "1401", "Désarmement" },
    { "1406", "Alarme annulée" },
    { "1455", "Échec d'armement automatique" },
    { "1570", "Zone shuntée" },
    { "1601", "Test de communication manuel" },
    { "1602", "Test de communication automatique" },
    { "3301", "Retour secteur" },
    { "3302", "Retour batterie" },
    { "3350", "Communication rétablie" },
    { "3351", "Ligne téléphonique rétablie" },
    { "3370", "Boucle rétablie" },
    { "3381", "Détecteur retrouvé" },
    { "3384", "Batterie détecteur rétablie" },
    { "3401", "Armement total" },
    { "3441", "Armement partiel (périmétrique)" },
    { "3570", "Fin de shuntage de zone" },
};
#define MEIAN_CID_TABLE_LEN (sizeof(MEIAN_CID_TABLE) / sizeof(MEIAN_CID_TABLE[0]))

static const char* meian_cid_label(const char *cid)
{
    for (size_t i = 0; i < MEIAN_CID_TABLE_LEN; i++) {
        if (strcmp(MEIAN_CID_TABLE[i].cid, cid) == 0) {
            return MEIAN_CID_TABLE[i].label;
        }
    }
    return "Évènement inconnu";
}

// Indique si le code Cid figure dans la table (sert à ne notifier par SMS que les évènements reconnus)
static bool meian_cid_exists(const char *cid)
{
    for (size_t i = 0; i < MEIAN_CID_TABLE_LEN; i++) {
        if (strcmp(MEIAN_CID_TABLE[i].cid, cid) == 0) {
            return true;
        }
    }
    return false;
}

// Codes Cid propres à cette installation à ne jamais notifier (ex: défaut de boucle structurel connu)
static const char* MEIAN_CID_IGNORED[] = { "1370", "1351", "3370" };
#define MEIAN_CID_IGNORED_LEN (sizeof(MEIAN_CID_IGNORED) / sizeof(MEIAN_CID_IGNORED[0]))

static bool meian_cid_is_ignored(const char *cid)
{
    for (size_t i = 0; i < MEIAN_CID_IGNORED_LEN; i++) {
        if (strcmp(MEIAN_CID_IGNORED[i], cid) == 0) {
            return true;
        }
    }
    return false;
}

// Déduit l'état interne à partir d'un code Cid (évènement Push ou code persisté en NVS)
static meian_state_t meian_state_from_cid(const char *cid)
{
    if (strcmp(cid, "1401") == 0) {
        return ALARM_STATE_DISARMED;
    } else if (strcmp(cid, "3401") == 0) {
        return ALARM_STATE_ARMED_AWAY;
    } else if (strcmp(cid, "3441") == 0) {
        return ALARM_STATE_ARMED_STAY;
    } else if (strcmp(cid, "1131") == 0 || strcmp(cid, "1132") == 0 ||
               strcmp(cid, "1133") == 0 || strcmp(cid, "1134") == 0) {
        return ALARM_STATE_TRIGGERED;
    }
    return ALARM_STATE_UNKNOWN;
}

// Inverse de meian_state_from_cid : code Cid représentatif d'un état, pour garder last_cid cohérent
// même quand l'état provient d'un GetAlarmStatus (polling) et non d'un évènement Push
static const char* meian_cid_from_state(meian_state_t alarm_state)
{
    switch (alarm_state) {
        case ALARM_STATE_DISARMED:   return "1401";
        case ALARM_STATE_ARMED_AWAY: return "3401";
        case ALARM_STATE_ARMED_STAY: return "3441";
        case ALARM_STATE_TRIGGERED:  return "1132";
        default:                     return NULL;
    }
}

// Décode une valeur typée du protocole Meian (ex: "NUM,4,4|1401", "S32,0,4|70", "GBA,16|50616C696572")
// vers du texte lisible : GBA est encodé en hexadécimal, les autres types ont leur valeur après le '|'
static void meian_decode_typed_value(const char *raw, char *out, size_t out_size)
{
    const char *pipe = strchr(raw, '|');
    if (pipe == NULL) {
        strlcpy(out, raw, out_size);
        return;
    }
    const char *value = pipe + 1;

    if (strncmp(raw, "GBA,", 4) == 0) {
        size_t hex_len = strlen(value);
        size_t max_bytes = out_size - 1;
        size_t i = 0;
        for (; i < hex_len / 2 && i < max_bytes; i++) {
            char byte_str[3] = { value[i * 2], value[i * 2 + 1], '\0' };
            out[i] = (char)strtol(byte_str, NULL, 16);
        }
        out[i] = '\0';
        return;
    }

    strlcpy(out, value, out_size);
}

// Extrait le contenu textuel d'une balise XML simple (ex: <Cid>1401</Cid> -> "1401")
static bool meian_xml_tag_value(const char *xml, const char *tag, char *out, size_t out_size)
{
    char open_tag[24];
    char close_tag[24];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (start == NULL) {
        return false;
    }
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
    if (end == NULL || end < start) {
        return false;
    }
    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

// Les réponses/évènements sont préfixés par "<Err>ERR|NN</Err>" avant même <Root> : on l'ignore ici
static const char* meian_strip_leading_err(const char *xml)
{
    const char *marker = "<Err>ERR|";
    if (strncmp(xml, marker, strlen(marker)) == 0) {
        const char *end = strstr(xml, "</Err>");
        if (end != NULL) {
            return end + strlen("</Err>");
        }
    }
    return xml;
}

// Le panneau notifie parfois le même évènement deux fois (ex: trames "@alA" et "!lmX" successives) :
// on ignore une répétition du même (Cid, Zone) survenant dans cette fenêtre pour éviter le double SMS
#define MEIAN_PUSH_DEDUP_WINDOW_MS 3000
static char s_last_event_key[24] = {0};
static TickType_t s_last_event_tick = 0;

// Analyse un évènement reçu sur le canal Push (<Root><Host><Alarm>...) et notifie les contacts
static void meian_handle_push_event(const char *raw_xml)
{
    const char *xml = meian_strip_leading_err(raw_xml);

    char raw_value[80];
    char cid[8] = {0};
    char content[64] = {0};
    char zone[8] = {0};
    char zone_name[48] = {0};

    if (!meian_xml_tag_value(xml, "Cid", raw_value, sizeof(raw_value))) {
        ESP_LOGW(TAG, "Évènement Push sans code Cid : %s", xml);
        return;
    }
    meian_decode_typed_value(raw_value, cid, sizeof(cid));

    if (meian_xml_tag_value(xml, "Content", raw_value, sizeof(raw_value))) {
        meian_decode_typed_value(raw_value, content, sizeof(content));
    }
    if (meian_xml_tag_value(xml, "Zone", raw_value, sizeof(raw_value))) {
        meian_decode_typed_value(raw_value, zone, sizeof(zone));
    }
    if (meian_xml_tag_value(xml, "ZoneName", raw_value, sizeof(raw_value))) {
        meian_decode_typed_value(raw_value, zone_name, sizeof(zone_name));
    }

    char event_key[24];
    snprintf(event_key, sizeof(event_key), "%s|%s", cid, zone);
    TickType_t now = xTaskGetTickCount();
    bool is_duplicate = (strcmp(event_key, s_last_event_key) == 0) &&
                         ((now - s_last_event_tick) * portTICK_PERIOD_MS < MEIAN_PUSH_DEDUP_WINDOW_MS);
    strlcpy(s_last_event_key, event_key, sizeof(s_last_event_key));
    s_last_event_tick = now;
    if (is_duplicate) {
        ESP_LOGD(TAG, "Évènement Meian [%s] reçu en double, ignoré.", cid);
        return;
    }

    if (meian_cid_is_ignored(cid)) {
        ESP_LOGI(TAG, "Évènement Meian [%s] ignoré (structurel à cette installation).", cid);
        return;
    }

    bool known = meian_cid_exists(cid);
    const char *label = meian_cid_label(cid);
    ESP_LOGW(TAG, "🔔 Évènement Meian [%s] %s (zone %s%s%s) - %s", cid, label,
             zone[0] ? zone : "-", zone_name[0] ? " " : "", zone_name, content);

    meian_state_t mapped_state = meian_state_from_cid(cid);
    if (mapped_state != ALARM_STATE_UNKNOWN) {
        s_current_state = mapped_state;
    }
    s_last_notified_state = s_current_state;
    meian_persist_last_cid(cid); // Survit à un redémarrage, exposé via meian_config_to_json

    if (!known) {
        return; // Évènement non répertorié : on ne dérange pas les contacts par SMS
    }

    char message[96];
    if (zone_name[0] != '\0') {
        snprintf(message, sizeof(message), "Alarme : %s (%s)", label, zone_name);
    } else {
        snprintf(message, sizeof(message), "Alarme : %s", label);
    }

    meian_broadcast_sms(message);
}

// Envoie la trame d'inscription au canal Push (<Root><Pair><Push>) ; le protocole impose seq=0 ici
static bool meian_push_register(int sock)
{
    char push_xml[128];
    snprintf(push_xml, sizeof(push_xml),
             "<Root><Pair><Push><Id>STR,%d|%s</Id><Err></Err></Push></Pair></Root>",
             (int)strlen(s_config.user), s_config.user);
    return meian_send_frame(sock, push_xml, 0);
}

// Lit une trame quelconque du canal Push. Retourne : 1 = XML reçu, 2 = ping keepalive du serveur,
// 0 = timeout (rien reçu), -1 = erreur/déconnexion
static int meian_push_read(int sock, char *rx_buffer, char *magic_out, char **xml_out, int *xml_len_out)
{
    int received = 0;
    while (received < 4) {
        int n = recv(sock, magic_out + received, 4 - received, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return received == 0 ? 0 : -1; // Timeout normal si rien n'a encore été reçu
            }
            return -1;
        } else if (n == 0) {
            return -1; // Connexion fermée par la centrale
        }
        received += n;
    }
    magic_out[4] = '\0';

    if (strncmp(magic_out, "%maI", 4) == 0) {
        return 2; // Ping keepalive envoyé par la centrale
    }
    if (strncmp(magic_out, "@ieM", 4) != 0 && strncmp(magic_out, "@alA", 4) != 0 &&
        strncmp(magic_out, "!lmX", 4) != 0) {
        ESP_LOGW(TAG, "Trame Push inconnue reçue (magic=%.4s).", magic_out);
        return -1;
    }

    memcpy(rx_buffer, magic_out, 4);
    int header_received = 4;
    while (header_received < 16) {
        int n = recv(sock, rx_buffer + header_received, 16 - header_received, 0);
        if (n <= 0) {
            return -1;
        }
        header_received += n;
    }

    char len_str[5] = {0};
    memcpy(len_str, rx_buffer + 4, 4);
    int body_len = atoi(len_str);
    if (body_len <= 0 || body_len >= RX_BUFFER_SIZE - 20) {
        ESP_LOGW(TAG, "Longueur de trame Push invalide : %d.", body_len);
        return -1;
    }

    int total_body = body_len + 4; // Corps XML + seq final (4 octets)
    int body_received = 0;
    while (body_received < total_body) {
        int n = recv(sock, rx_buffer + 16 + body_received, total_body - body_received, 0);
        if (n <= 0) {
            return -1;
        }
        body_received += n;
    }

    char *xml_ptr = rx_buffer + 16;
    if (strncmp(magic_out, "!lmX", 4) != 0) { // "!lmX" est le seul format non chiffré
        meian_xor_cipher(xml_ptr, body_len);
    }
    xml_ptr[body_len] = '\0';

    *xml_out = xml_ptr;
    *xml_len_out = body_len;
    return 1;
}

/**
 * Tâche FreeRTOS : maintient la connexion au canal Push et traite les évènements en temps réel
 */
void meian_push_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Démarrage du moniteur Push Meian...");
    char *rx_buffer = malloc(RX_BUFFER_SIZE);
    if (rx_buffer == NULL) {
        ESP_LOGE(TAG, "Échec d'allocation mémoire pour le moniteur Push.");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (!s_config.enabled || !s_config.has_ip || !s_config.has_user) {
            vTaskDelay(pdMS_TO_TICKS(MEIAN_PUSH_RECONNECT_MS));
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Impossible de créer la socket Push: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(MEIAN_PUSH_RECONNECT_MS));
            continue;
        }

        struct timeval timeout;
        timeout.tv_sec = MEIAN_PUSH_RECV_TIMEOUT_S;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(s_config.ip);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(ALARM_PORT);

        if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0 || !meian_push_register(sock)) {
            ESP_LOGW(TAG, "Échec de connexion/inscription au canal Push Meian, nouvel essai dans %d s.",
                     MEIAN_PUSH_RECONNECT_MS / 1000);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(MEIAN_PUSH_RECONNECT_MS));
            continue;
        }

        ESP_LOGI(TAG, "Canal Push Meian connecté, en attente d'évènements...");
        TickType_t last_keepalive = xTaskGetTickCount();

        while (s_config.enabled) {
            char magic[5];
            char *xml_response = NULL;
            int xml_len = 0;
            int result = meian_push_read(sock, rx_buffer, magic, &xml_response, &xml_len);

            if (result == -1) {
                ESP_LOGW(TAG, "Connexion Push perdue, reconnexion...");
                break;
            } else if (result == 1) {
                if (strncmp(magic, "@ieM", 4) == 0) {
                    if (!meian_response_ok(xml_response)) {
                        ESP_LOGW(TAG, "Inscription au canal Push refusée par la centrale.");
                        break;
                    }
                    ESP_LOGI(TAG, "Inscription au canal Push confirmée par la centrale.");
                    meian_sync_alarm_state(); // Rattrape les évènements manqués pendant la déconnexion
                } else {
                    meian_handle_push_event(xml_response);
                }
            }
            // result == 0 (timeout) ou 2 (ping du serveur) : rien de particulier à faire

            if ((xTaskGetTickCount() - last_keepalive) * portTICK_PERIOD_MS >= MEIAN_PUSH_KEEPALIVE_MS) {
                if (send(sock, "%maI", 4, 0) != 4) {
                    ESP_LOGW(TAG, "Échec de l'envoi du keepalive Push.");
                    break;
                }
                last_keepalive = xTaskGetTickCount();
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(MEIAN_PUSH_RECONNECT_MS));
    }
}

/**
 * Synchronise une fois l'état de l'alarme (GetAlarmStatus) et notifie les contacts si besoin.
 * Le canal Push ne fournit que les changements à venir, jamais l'état initial : cette fonction
 * comble ce trou, à appeler au démarrage (si déjà activé) et lors du passage à enabled=true.
 */
void meian_sync_alarm_state(void) {
    if (!s_config.enabled) {
        return;
    }

    // L'état précédent réellement connu est celui persisté en NVS (last_cid), pas une variable
    // volatile remise à zéro à chaque redémarrage : sinon on notifierait à tort à chaque boot
    meian_state_t previous_known_state = s_has_last_cid ? meian_state_from_cid(s_last_cid) : ALARM_STATE_UNKNOWN;

    ESP_LOGI(TAG, "Synchronisation de l'état de l'alarme...");
    execute_meian_transaction(XML_REQ_STATUS, false);

    if (s_current_state != previous_known_state) {
        meian_notify_contacts_state_change(s_current_state);
    }
    s_last_notified_state = s_current_state;

    const char *matching_cid = meian_cid_from_state(s_current_state);
    if (matching_cid != NULL) {
        meian_persist_last_cid(matching_cid); // Garde last_status_cid/label cohérents même après un polling
    }
}

#define MEIAN_SMS_POLL_INTERVAL_MS 10000
#define MEIAN_SMS_PIN_MAX_LEN      7  // 4 à 6 chiffres + terminateur nul
#define MEIAN_SMS_CMD_MAX_LEN      16
#define MEIAN_SMS_CMD_PREFIX       "#PWD"


// Analyse un SMS au format "#PWD<pin>#<CMD>" ; renvoie false si le contenu ne correspond pas à ce format
static bool meian_parse_sms_command(const char *text, char *pin_out, size_t pin_out_size, char *cmd_out, size_t cmd_out_size)
{
    size_t prefix_len = strlen(MEIAN_SMS_CMD_PREFIX);
    if (strncmp(text, MEIAN_SMS_CMD_PREFIX, prefix_len) != 0) {
        return false;
    }

    const char *pin_start = text + prefix_len;
    const char *hash = strchr(pin_start, '#');
    if (hash == NULL) {
        return false;
    }

    size_t pin_len = (size_t)(hash - pin_start);
    if (pin_len < 4 || pin_len > 6 || pin_len >= pin_out_size) {
        return false;
    }
    for (size_t i = 0; i < pin_len; i++) {
        if (pin_start[i] < '0' || pin_start[i] > '9') {
            return false;
        }
    }

    const char *cmd_start = hash + 1;
    size_t cmd_len = strlen(cmd_start);
    while (cmd_len > 0 && (cmd_start[cmd_len - 1] == '\r' || cmd_start[cmd_len - 1] == '\n' || cmd_start[cmd_len - 1] == ' ')) {
        cmd_len--;
    }
    if (cmd_len == 0 || cmd_len >= cmd_out_size) {
        return false;
    }

    memcpy(pin_out, pin_start, pin_len);
    pin_out[pin_len] = '\0';
    memcpy(cmd_out, cmd_start, cmd_len);
    cmd_out[cmd_len] = '\0';
    return true;
}

// Vérifie que l'expéditeur est un contact connu, avec un code PIN défini et correspondant
static bool meian_sender_authorized(const char *sender, const char *pin)
{
    contact_info_t contacts[CONTACTS_MAX_COUNT];
    int count = contacts_get_all(contacts, CONTACTS_MAX_COUNT);
    for (int i = 0; i < count; i++) {
        if (strcmp(contacts[i].phone, sender) == 0) {
            return contacts[i].has_pin && strcmp(contacts[i].pin, pin) == 0;
        }
    }
    return false;
}

static void meian_execute_sms_command(const char *cmd, const char *sender)
{
    bool status_changed = false;
    bool success = false;
    if (strcmp(cmd, "ARM") == 0) {
        if (s_current_state != ALARM_STATE_ARMED_AWAY) {
            status_changed = true;
        }
        success = arm();
    } else if (strcmp(cmd, "DISARM") == 0) {
        if (s_current_state != ALARM_STATE_DISARMED) {
            status_changed = true;
        }
        success = disarm();
    } else if (strcmp(cmd, "CHECK") == 0) {
        success = state();
    } else if (strcmp(cmd, "STAY") == 0) {
        if (s_current_state != ALARM_STATE_ARMED_STAY) {
            status_changed = true;
        }
        success = perimeter();
    } else {
        ESP_LOGW(TAG, "Commande SMS inconnue : %s", cmd);
        return;
    }
    char message[64];
    if (!success) {
        snprintf(message, sizeof(message), "Échec de la commande : %s", cmd);
        board_send_sms(global_modem, sender, message);
        return;
    }
    if (strcmp(cmd, "CHECK") == 0) {
        // Confirme par SMS à l'expéditeur l'état résultant de la commande (utile notamment pour CHECK)
        snprintf(message, sizeof(message), "Alarme : %s", meian_state_label(s_current_state));
        board_send_sms(global_modem, sender, message);
    } else if (!status_changed) {
        // Dans les autres cas il y a un changement de statut via le push de l'alarme sauf si le statut est inchangé
        // On renotifie l'état actuel de l'alarme par SMS
        snprintf(message, sizeof(message), "Alarme : %s", meian_state_label(s_current_state));
        board_send_sms(global_modem, sender, message);
    }
}

// Traite un élément du tableau JSON renvoyé par board_list_unread_sms_json ; marque le SMS comme lu
// s'il s'agit d'une commande (autorisée ou non), laisse intacts (toujours non lus) les SMS hors format attendu
static void meian_process_sms_item(cJSON *item)
{
    cJSON *index_item = cJSON_GetObjectItem(item, "index");
    cJSON *sender_item = cJSON_GetObjectItem(item, "sender");
    cJSON *text_item = cJSON_GetObjectItem(item, "text");
    if (!cJSON_IsNumber(index_item) || !cJSON_IsString(sender_item) || !cJSON_IsString(text_item)) {
        return;
    }

    char pin[MEIAN_SMS_PIN_MAX_LEN] = {0};
    char cmd[MEIAN_SMS_CMD_MAX_LEN] = {0};
    if (!meian_parse_sms_command(text_item->valuestring, pin, sizeof(pin), cmd, sizeof(cmd))) {
        ESP_LOGI(TAG, "Commande SMS '%s' non reconnue depuis %s", text_item->valuestring, sender_item->valuestring);
        return; // Pas un SMS de commande : on le laisse non lu, intact pour l'utilisateur
    }

    const char *sender = sender_item->valuestring;
    if (meian_sender_authorized(sender, pin)) {
        ESP_LOGI(TAG, "Commande SMS '%s' autorisée depuis %s", cmd, sender);
        meian_execute_sms_command(cmd, sender);
    } else {
        ESP_LOGW(TAG, "Commande SMS refusée depuis %s (expéditeur inconnu ou code PIN invalide)", sender);
    }

    int index = index_item->valueint;
    if (board_mark_sms_read(global_modem, index) != ESP_OK) {
        // Échec de marquage comme lu
        ESP_LOGW(TAG, "Échec de marquage du SMS %d", index);
    }
}

/**
 * Tâche FreeRTOS : surveille les SMS non lus et exécute les commandes autorisées
 * (uniquement lorsque la configuration Meian est activée)
 */
void meian_sms_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Démarrage du moniteur de SMS Meian...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MEIAN_SMS_POLL_INTERVAL_MS));

        if (!s_config.enabled || global_modem == NULL) {
            continue;
        }

        char *json_array = board_list_unread_sms_json(global_modem);
        if (json_array == NULL) {
            continue;
        }

        cJSON *arr = cJSON_Parse(json_array);
        free(json_array);
        if (arr == NULL) {
            continue;
        }

        cJSON *item = NULL;
        cJSON_ArrayForEach(item, arr) {
            meian_process_sms_item(item);
        }
        cJSON_Delete(arr);
    }
}


