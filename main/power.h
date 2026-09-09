#ifndef POWER_H_
#define POWER_H_

// LilyGo T-A7670G power monitoring pins (diviseur de tension matériel : la lecture réelle est doublée)
#define POWER_BATTERY_ADC_PIN 35
#define POWER_SOLAR_ADC_PIN   36 // Non connecté sur toutes les révisions de carte
#define POWER_ON_PIN           12 // Doit rester HIGH, sinon la carte s'éteint sur batterie sans USB

/**
 * @brief Maintient l'alimentation générale de la carte active (indispensable en fonctionnement sur
 *        batterie, sans USB branché) ; à appeler le plus tôt possible dans app_main()
 */
void power_hold_board_on(void);

/**
 * @brief Lit la tension batterie en mV (moyenne sur plusieurs échantillons)
 * @return Tension en mV, ou -1 en cas d'échec de lecture ADC
 */
int power_get_battery_mv(void);

/**
 * @brief Lit la tension du panneau solaire en mV (moyenne sur plusieurs échantillons)
 * @return Tension en mV, ou -1 en cas d'échec de lecture ADC (ou pin non connectée sur cette révision)
 */
int power_get_solar_mv(void);

/**
 * @brief Démarre une tâche qui vérifie la tension batterie toutes les minutes et envoie un SMS à
 *        tous les contacts lors du passage sur batterie (< 3800mV) puis du retour secteur (> 3950mV).
 *        L'état d'alerte est persisté en NVS pour survivre à un redémarrage (bascule secteur/batterie).
 *        À appeler une fois au démarrage, une fois le modem et les contacts initialisés.
 */
void power_start_battery_monitor(void);

#endif // POWER_H_
