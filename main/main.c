#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "wifi_prov.h"
#include "esp_modem_api.h"
#include "board.h" // Contient vos signatures de fonctions SMS


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
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    global_modem = initialize_modem();
    if (global_modem == NULL) {
        ESP_LOGE(TAG, "Modem initialization failed");
        return;
    }

    
    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(MDNS_HOST_NAME);

    ESP_ERROR_CHECK(wifi_prov_connect());
    ESP_ERROR_CHECK(start_rest_server());
}
