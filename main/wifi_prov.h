#ifndef WIFI_PROV_H_
#define WIFI_PROV_H_

#include "esp_err.h"

/**
 * @brief Connecte le WiFi en mode STA avec les identifiants enregistrés en NVS.
 *        Si aucun identifiant n'est enregistré (ou si la connexion échoue), démarre
 *        un point d'accès "ESP32-Setup" avec un formulaire web (http://192.168.4.1/)
 *        permettant de saisir un nouveau SSID/mot de passe ; l'appareil redémarre
 *        automatiquement une fois les identifiants enregistrés.
 * @note Cette fonction ne retourne que si la connexion STA réussit (ESP_OK) ;
 *       en mode portail de configuration, elle bloque indéfiniment jusqu'au redémarrage.
 */
esp_err_t wifi_prov_connect(void);

#endif // WIFI_PROV_H_
