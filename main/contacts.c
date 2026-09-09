#include <string.h>
#include "contacts.h"
#include "nvs.h"
#include "cJSON.h"

#define CONTACTS_NVS_NAMESPACE "contacts"
#define CONTACTS_NVS_KEY       "store"

typedef struct {
    bool used;
    int id;
    char name[CONTACTS_NAME_MAX_LEN];
    char phone[CONTACTS_PHONE_MAX_LEN];
} contact_entry_t;

typedef struct {
    contact_entry_t entries[CONTACTS_MAX_COUNT];
    int next_id;
} contacts_store_t;

static contacts_store_t s_store;

// Persiste l'intégralité de la liste en un seul blob NVS
static esp_err_t contacts_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONTACTS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, CONTACTS_NVS_KEY, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t contacts_init(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.next_id = 1;

    nvs_handle_t handle;
    if (nvs_open(CONTACTS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK; // Aucun contact enregistré pour l'instant : liste vide
    }

    size_t size = sizeof(s_store);
    if (nvs_get_blob(handle, CONTACTS_NVS_KEY, &s_store, &size) != ESP_OK) {
        // Rien de stocké, ou format incompatible avec une version précédente : on repart à vide
        memset(&s_store, 0, sizeof(s_store));
        s_store.next_id = 1;
    }
    nvs_close(handle);
    return ESP_OK;
}

char* contacts_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }

    for (int i = 0; i < CONTACTS_MAX_COUNT; i++) {
        if (!s_store.entries[i].used) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", s_store.entries[i].id);
        cJSON_AddStringToObject(item, "name", s_store.entries[i].name);
        cJSON_AddStringToObject(item, "phone", s_store.entries[i].phone);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json_str;
}

int contacts_get_all(contact_info_t *out, int max_count)
{
    int count = 0;
    for (int i = 0; i < CONTACTS_MAX_COUNT && count < max_count; i++) {
        if (!s_store.entries[i].used) {
            continue;
        }
        out[count].id = s_store.entries[i].id;
        strlcpy(out[count].name, s_store.entries[i].name, sizeof(out[count].name));
        strlcpy(out[count].phone, s_store.entries[i].phone, sizeof(out[count].phone));
        count++;
    }
    return count;
}

esp_err_t contacts_add(const char *name, const char *phone, int *out_id)
{
    if (name == NULL || phone == NULL || name[0] == '\0' || phone[0] == '\0' ||
        strlen(name) >= CONTACTS_NAME_MAX_LEN || strlen(phone) >= CONTACTS_PHONE_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    int free_slot = -1;
    for (int i = 0; i < CONTACTS_MAX_COUNT; i++) {
        if (!s_store.entries[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1) {
        return ESP_ERR_NO_MEM; // Limite de CONTACTS_MAX_COUNT atteinte
    }

    s_store.entries[free_slot].used = true;
    s_store.entries[free_slot].id = s_store.next_id++;
    strlcpy(s_store.entries[free_slot].name, name, sizeof(s_store.entries[free_slot].name));
    strlcpy(s_store.entries[free_slot].phone, phone, sizeof(s_store.entries[free_slot].phone));

    if (out_id != NULL) {
        *out_id = s_store.entries[free_slot].id;
    }
    return contacts_save();
}

esp_err_t contacts_update(int id, const char *name, const char *phone)
{
    if (name == NULL || phone == NULL || name[0] == '\0' || phone[0] == '\0' ||
        strlen(name) >= CONTACTS_NAME_MAX_LEN || strlen(phone) >= CONTACTS_PHONE_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < CONTACTS_MAX_COUNT; i++) {
        if (s_store.entries[i].used && s_store.entries[i].id == id) {
            strlcpy(s_store.entries[i].name, name, sizeof(s_store.entries[i].name));
            strlcpy(s_store.entries[i].phone, phone, sizeof(s_store.entries[i].phone));
            return contacts_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t contacts_delete(int id)
{
    int found_index = -1;
    for (int i = 0; i < CONTACTS_MAX_COUNT; i++) {
        if (s_store.entries[i].used && s_store.entries[i].id == id) {
            found_index = i;
            break;
        }
    }
    if (found_index == -1) {
        return ESP_ERR_NOT_FOUND;
    }

    // Recompacte la liste et réindexe les contacts restants de façon contiguë à partir de 1
    contact_entry_t remaining[CONTACTS_MAX_COUNT];
    memset(remaining, 0, sizeof(remaining));
    int count = 0;
    for (int i = 0; i < CONTACTS_MAX_COUNT; i++) {
        if (i == found_index || !s_store.entries[i].used) {
            continue;
        }
        remaining[count] = s_store.entries[i];
        remaining[count].id = count + 1;
        count++;
    }

    memset(s_store.entries, 0, sizeof(s_store.entries));
    memcpy(s_store.entries, remaining, sizeof(contact_entry_t) * count);
    s_store.next_id = count + 1;

    return contacts_save();
}
