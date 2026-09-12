#ifndef BOARD_H_
#define BOARD_H_

// 1. Dépendances requises pour les types de données du modem et de l'ESP32
#include "esp_modem_api.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_modem_config.h"

// LilyGo T-A7670G ESP32 Hardware Pin Map
#define BOARD_MODEM_PWRKEY      4
#define BOARD_MODEM_DTR         25
#define BOARD_MODEM_TX          26
#define BOARD_MODEM_RX          27

esp_modem_dce_t* initialize_modem(void);
esp_err_t board_send_sms(esp_modem_dce_t *dce, const char *phone_number, const char *message);
char* board_list_sms_json(esp_modem_dce_t *dce); // Tableau JSON structuré des SMS (à libérer avec free()), NULL si échec
char* board_list_unread_sms_json(esp_modem_dce_t *dce); // Idem, limité aux non lus, sans marquer les messages comme lus (AT+CMGL)
esp_err_t board_read_sms(esp_modem_dce_t *dce, int index, char *out_buf, size_t out_buf_size);
char* board_read_sms_json(esp_modem_dce_t *dce, int index); // Objet JSON structuré du SMS (à libérer avec free()), NULL si échec
esp_err_t board_mark_sms_read(esp_modem_dce_t *dce, int index); // Marque un SMS comme lu sans exploiter son contenu
esp_err_t board_delete_sms(esp_modem_dce_t *dce, int index);
char* board_get_status_json(esp_modem_dce_t *dce); // ⚠️ ATTENTION : Libérer impérativement la mémoire allouée par cJSON

extern esp_modem_dce_t *global_modem;

#endif // BOARD_H_