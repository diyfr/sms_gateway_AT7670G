#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_prov.h"
#include "esp_modem_api.h"
#include "board.h" // Contient vos signatures de fonctions SMS
#include "power.h"
#include "contacts.h"
#include "meian.h"


esp_modem_dce_t *global_modem = NULL;



#define MDNS_INSTANCE "dashboard web server"
#define MDNS_HOST_NAME CONFIG_EXAMPLE_MDNS_HOST_NAME

[[maybe_unused]] static const char *TAG = "main";

extern esp_err_t start_rest_server(void);

static void initialise_mdns(void)
{
    mdns_init();
    mdns_hostname_set(MDNS_HOST_NAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    mdns_txt_item_t serviceTxtData[] = {
        {"chip", CONFIG_IDF_TARGET},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

void app_main(void)
{
    power_hold_board_on(); // Doit être fait avant tout le reste pour rester alimenté sur batterie
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(contacts_init());
    ESP_ERROR_CHECK(meian_config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    global_modem = initialize_modem();
    if (global_modem == NULL) {
        ESP_LOGE(TAG, "Modem initialization failed");
        return;
    }
    power_start_battery_monitor();

    
    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(MDNS_HOST_NAME);

    ESP_ERROR_CHECK(wifi_prov_connect());
    ESP_ERROR_CHECK(start_rest_server());

    meian_sync_alarm_state(); // Synchro ponctuelle si déjà activé ; le canal Push prend le relais ensuite

    xTaskCreate(meian_sms_monitor_task, "meian_sms_task", 4096, NULL, 5, NULL);
    xTaskCreate(meian_push_monitor_task, "meian_push_task", 4096, NULL, 5, NULL);
}
