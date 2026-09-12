#ifndef MEIAN_H_
#define MEIAN_H_

#include <stdbool.h>
#include "esp_err.h"

bool arm(void);
bool disarm(void);
bool perimeter(void);
bool state(void);

void meian_xor_cipher(char *data, int len);
void parse_alarm_status(const char *xml_response);
void parse_zones_status(char *xml_response);
bool execute_meian_transaction(const char *xml_payload, bool is_zone_query);

/**
 * @brief Synchronise une fois l'état de l'alarme (GetAlarmStatus) et notifie les contacts si l'état a
 * changé depuis la dernière notification. Ne fait rien si enabled == false. À appeler au démarrage
 * (si déjà activé) et lors de l'activation via meian_config_set : le canal Push ne fournit que les
 * changements d'état à venir, jamais l'état initial.
 */
void meian_sync_alarm_state(void);

/**
 * @brief Tâche FreeRTOS : maintient une connexion persistante au canal Push (<Pair><Push>) de la
 * centrale et traite en temps réel les évènements reçus (changement d'état, alarme, etc.)
 */
void meian_push_monitor_task(void *pvParameters);

/**
 * @brief Tâche FreeRTOS : surveille périodiquement les SMS reçus et exécute les commandes
 * autorisées au format "#PWD<pin>#<CMD>" (CMD = ARM, DISARM ou CHECK), uniquement lorsque
 * la configuration Meian est activée (voir meian_config_set)
 */
void meian_sms_monitor_task(void *pvParameters);

#define MEIAN_IP_MAX_LEN 46 // Marge suffisante pour une adresse IPv4 ou IPv6 textuelle
#define MEIAN_USER_MAX_LEN 32
#define MEIAN_PASSWORD_MAX_LEN 32

typedef struct {
    bool enabled;
    bool has_ip; // false => ip vide (autorisé uniquement si enabled == false)
    char ip[MEIAN_IP_MAX_LEN];
    bool has_user; // false => user vide (autorisé uniquement si enabled == false)
    char user[MEIAN_USER_MAX_LEN];
    bool has_password; // false => password vide (autorisé uniquement si enabled == false)
    char password[MEIAN_PASSWORD_MAX_LEN];
} meian_config_t;

/**
 * @brief Charge la configuration Meian persistée en NVS (à appeler une fois au démarrage)
 */
esp_err_t meian_config_init(void);

/**
 * @brief Définit enabled/ip/user/password et persiste immédiatement en NVS
 * @param ip Adresse IP ; peut être NULL ou vide uniquement si enabled == false
 * @param user Identifiant de connexion à la centrale ; peut être NULL ou vide uniquement si enabled == false
 * @param password Mot de passe de connexion à la centrale ; peut être NULL ou vide uniquement si enabled == false
 * @return ESP_OK, ESP_ERR_INVALID_ARG (champ manquant alors que enabled == true, ou format invalide)
 */
esp_err_t meian_config_set(bool enabled, const char *ip, const char *user, const char *password);

/**
 * @brief Copie la configuration courante dans out
 */
void meian_config_get(meian_config_t *out);

/**
 * @brief Sérialise la configuration courante en JSON : {"enabled":bool,"ip":string|null,"user":string|null,
 * "has_password":bool,"last_status_cid":string|null,"last_status_label":string|null}
 * Le mot de passe lui-même n'est jamais renvoyé, seule sa présence est indiquée. last_status_cid/label
 * reflètent le dernier évènement (Cid) reçu via le canal Push, persisté en NVS (survit à un redémarrage).
 * @return char* Chaîne JSON allouée (à libérer avec free()), ou NULL en cas d'échec d'allocation
 */
char* meian_config_to_json(void);

/**
 * @brief Sérialise un statut public minimal (sans IP/identifiants), pour affichage sur la page
 * d'accueil sans authentification : {"enabled":bool,"status_label":string|null}
 * @return char* Chaîne JSON allouée (à libérer avec free()), ou NULL en cas d'échec d'allocation
 */
char* meian_status_to_json(void);

#endif // MEIAN_H_