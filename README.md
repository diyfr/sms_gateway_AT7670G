# A7670G SMS Gateway

Passerelle SMS basée sur ESP32 + modem cellulaire A7670G (compatible SIM7600), exposant une API REST pour envoyer, lire, lister et supprimer des SMS, ainsi que consulter l'état du modem et de la carte.

## Matériel

Carte type **LilyGo T-A7670G** (ESP32 + modem A7670G) :

| Fonction | GPIO |
|---|---|
| PWRKEY (alimentation modem) | 4 |
| DTR | 25 |
| UART TX (vers modem) | 26 |
| UART RX (depuis modem) | 27 |

## Fonctionnalités

- **Provisioning WiFi** : au premier démarrage (ou si les identifiants enregistrés ne fonctionnent plus), l'appareil ouvre un point d'accès WiFi `ESP32-Setup` avec un formulaire web (`http://192.168.4.1/`) pour saisir le SSID/mot de passe du réseau. Les identifiants sont sauvegardés en NVS et l'appareil redémarre pour s'y connecter.
- **Envoi / lecture / suppression de SMS** via des commandes AT (mode texte).
- **Tableau de bord web** intégré au firmware (`GET /`), affichant en direct les informations système et l'état du modem.
- **API REST** (`/api/v1/*`) protégée par une clé API (`X-API-Key`), avec mDNS (`<hostname>.local`) pour la découverte sur le réseau local.
- **Statut détaillé du modem** : signal, enregistrement réseau, type de réseau (2G/3G/4G), SMSC, horloge synchronisée via le réseau (NITZ), identité du modem (fabricant, modèle, révision, IMEI).

## Routes API

Toutes les routes commencent par `/api/v1`. Le détail complet avec exemples de requêtes est disponible dans [api.http](api.http).

### Publiques (aucune protection)

| Méthode | Route | Description |
|---|---|---|
| GET | `/` | Tableau de bord HTML embarqué |
| GET | `/system/info` | Infos système (chip, version IDF, version app...) |
| GET | `/board/status` | État du modem (signal, réseau, SMS en attente, horloge...) |

### Protégées (en-tête `X-API-Key` requis)

| Méthode | Route | Description |
|---|---|---|
| POST | `/sms/send` | Envoyer un SMS (`{"to": "...", "text": "..."}`) |
| GET | `/sms/list` | Lister les SMS stockés sur la SIM (tableau JSON structuré) |
| GET | `/sms/read?index=N` | Lire un SMS précis |
| DELETE | `/sms/delete?index=N` | Supprimer un SMS précis |
| POST | `/system/reboot` | Redémarrer l'appareil |

## Configuration

Via `idf.py menuconfig` → **Example Configuration** :

- **mDNS Host Name** (`CONFIG_EXAMPLE_MDNS_HOST_NAME`, défaut `dashboard`)
- **Clé API REST** (`CONFIG_REST_API_KEY`, défaut `changeme`) — **à changer impérativement avant toute mise en production**

Un fichier `.env` (`API_KEY=...`) est utilisé par [api.http](api.http) (extension REST Client) pour les tests, il doit correspondre à `CONFIG_REST_API_KEY`.

## Build & flash

Projet ESP-IDF (v6.1, cible `esp32`) :

```powershell
idf.py build
idf.py -p <PORT> flash monitor
```

## Structure du projet

```
main/
  main.c          # Point d'entrée, init NVS/WiFi/mDNS, démarrage du serveur REST
  board.c/.h      # Pilotage du modem (AT commands), parsing des réponses
  wifi_prov.c/.h  # Provisioning WiFi (SoftAP + portail de configuration)
  rest_server.c   # Serveur HTTP et endpoints /api/v1/*
partitions_example.csv  # Table de partitions (nvs, phy_init, factory)
api.http          # Requêtes de test (extension VS Code REST Client)
```
