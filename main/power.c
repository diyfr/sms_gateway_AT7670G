#include <stdbool.h>
#include <string.h>
#include "power.h"
#include "board.h"
#include "contacts.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

// Mapping ESP32 classique : GPIO35 = ADC1_CH7, GPIO36 = ADC1_CH0
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_7
#define SOLAR_ADC_CHANNEL   ADC_CHANNEL_0
#define ADC_SAMPLE_COUNT    8

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_adc_initialized = false;

void power_hold_board_on(void)
{
    // gpio_set_direction() seul ne suffit pas de façon fiable sur cette pin (voir
    // github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/220) : il faut passer par gpio_config().
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << POWER_ON_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(POWER_ON_PIN, 1);
}

// Initialise l'unité ADC1 (une seule fois) pour les deux canaux batterie/solaire
static bool power_adc_init(void)
{
    if (s_adc_initialized) {
        return s_adc_handle != NULL;
    }
    s_adc_initialized = true;

    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init_config, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Échec d'initialisation de l'unité ADC1");
        s_adc_handle = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_config);
    adc_oneshot_config_channel(s_adc_handle, SOLAR_ADC_CHANNEL, &chan_config);

    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &s_adc_cali_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Calibration ADC indisponible, conversion mV approximative");
        s_adc_cali_handle = NULL;
    }

    return true;
}

// Lit la tension moyenne (mV) sur un canal, puis double la valeur (diviseur de tension matériel)
static int power_read_channel_mv(adc_channel_t channel)
{
    if (!power_adc_init()) {
        return -1;
    }

    int sum_mv = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, channel, &raw) != ESP_OK) {
            return -1;
        }
        int mv = 0;
        if (s_adc_cali_handle != NULL && adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &mv) == ESP_OK) {
            sum_mv += mv;
        } else {
            sum_mv += (raw * 3300) / 4095; // Repli approximatif si la calibration échoue
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return (sum_mv / ADC_SAMPLE_COUNT) * 2;
}

int power_get_battery_mv(void)
{
    return power_read_channel_mv(BATTERY_ADC_CHANNEL);
}

int power_get_solar_mv(void)
{
    return power_read_channel_mv(SOLAR_ADC_CHANNEL);
}

#define BATTERY_LOW_MV        3800
#define BATTERY_RESTORED_MV   3950
#define BATTERY_MIN_PLAUSIBLE_MV 3000 // En dessous : probablement pas de batterie connectée, on ignore
#define BATTERY_MONITOR_PERIOD_MS (60 * 1000)
#define POWER_NVS_NAMESPACE   "power"
#define POWER_NVS_KEY_ON_BATT "on_batt"

// Relit l'état "alerte batterie déjà envoyée" persisté en NVS (survit à un redémarrage)
static bool power_load_on_battery_flag(void)
{
    nvs_handle_t handle;
    if (nvs_open(POWER_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, POWER_NVS_KEY_ON_BATT, &value);
    nvs_close(handle);
    return (err == ESP_OK) && (value != 0);
}

static void power_save_on_battery_flag(bool on_battery)
{
    nvs_handle_t handle;
    if (nvs_open(POWER_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u8(handle, POWER_NVS_KEY_ON_BATT, on_battery ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
}

// Envoie le message donné à tous les contacts enregistrés
static void power_notify_contacts(const char *message)
{
    if (global_modem == NULL) {
        ESP_LOGW(TAG, "Modem indisponible, notification '%s' non envoyée", message);
        return;
    }

    contact_info_t contacts[CONTACTS_MAX_COUNT];
    int count = contacts_get_all(contacts, CONTACTS_MAX_COUNT);
    for (int i = 0; i < count; i++) {
        board_send_sms(global_modem, contacts[i].phone, message);
    }
}

static void battery_monitor_task(void *arg)
{
    bool on_battery = power_load_on_battery_flag();

    while (1) {
        int battery_mv = power_get_battery_mv();
        if (battery_mv > 0) {
            if (!on_battery && battery_mv < BATTERY_LOW_MV && battery_mv > BATTERY_MIN_PLAUSIBLE_MV) {
                ESP_LOGW(TAG, "Tension batterie basse (%d mV) : passage sur batterie", battery_mv);
                power_notify_contacts("Gateway sur batterie");
                on_battery = true;
                power_save_on_battery_flag(true);
            } else if (on_battery && battery_mv > BATTERY_RESTORED_MV) {
                ESP_LOGI(TAG, "Tension batterie rétablie (%d mV) : retour secteur", battery_mv);
                power_notify_contacts("Gateway sur secteur");
                on_battery = false;
                power_save_on_battery_flag(false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_MONITOR_PERIOD_MS));
    }
}

void power_start_battery_monitor(void)
{
    // Pile généreuse : board_send_sms passe par des appels C++ profonds (esp_modem)
    xTaskCreate(battery_monitor_task, "batt_monitor", 6144, NULL, 3, NULL);
}
