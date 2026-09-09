#ifndef CONTACTS_H_
#define CONTACTS_H_

#include "esp_err.h"

#define CONTACTS_MAX_COUNT     5
#define CONTACTS_NAME_MAX_LEN  32
#define CONTACTS_PHONE_MAX_LEN 20

typedef struct {
    int id;
    char name[CONTACTS_NAME_MAX_LEN];
    char phone[CONTACTS_PHONE_MAX_LEN];
} contact_info_t;

/**
 * @brief Charge les contacts persistés en NVS (à appeler une fois au démarrage, avant toute autre fonction)
 */
esp_err_t contacts_init(void);

/**
 * @brief Liste tous les contacts sous forme de tableau JSON : [{"id","name","phone"}, ...]
 * @return char* Chaîne JSON allouée (à libérer avec free()), ou NULL en cas d'échec d'allocation
 */
char* contacts_list_json(void);

/**
 * @brief Copie tous les contacts enregistrés dans out (taille max_count), sans passer par du JSON
 * @return Nombre de contacts copiés
 */
int contacts_get_all(contact_info_t *out, int max_count);

/**
 * @brief Ajoute un contact et persiste immédiatement en NVS
 * @param out_id Rempli avec l'id attribué au contact en cas de succès (peut être NULL)
 * @return ESP_OK, ESP_ERR_INVALID_ARG (nom/numéro invalide), ESP_ERR_NO_MEM (liste pleine)
 */
esp_err_t contacts_add(const char *name, const char *phone, int *out_id);

/**
 * @brief Modifie un contact existant et persiste immédiatement en NVS
 * @return ESP_OK, ESP_ERR_INVALID_ARG (nom/numéro invalide), ESP_ERR_NOT_FOUND (id inconnu)
 */
esp_err_t contacts_update(int id, const char *name, const char *phone);

/**
 * @brief Supprime un contact et persiste immédiatement en NVS
 * @return ESP_OK, ESP_ERR_NOT_FOUND (id inconnu)
 */
esp_err_t contacts_delete(int id);

#endif // CONTACTS_H_
