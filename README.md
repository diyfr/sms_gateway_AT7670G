# A7670G SMS Gateway

Passerelle SMS basée sur ESP32 + modem cellulaire A7670G (compatible SIM7600), exposant une API REST pour envoyer, lire, lister et supprimer des SMS, consulter l'état du modem/de la carte (dont batterie/solaire), et gérer un carnet de contacts.

## Matériel

Carte type **LilyGo T-A7670G** (ESP32 + modem A7670G) :

| Fonction | GPIO |
|---|---|
| PWRKEY (alimentation modem) | 4 |
| DTR | 25 |
| UART TX (vers modem) | 26 |
| UART RX (depuis modem) | 27 |
| Alimentation générale de la carte (`BOARD_POWERON_PIN`) | 12 |
| ADC batterie (tension réelle = valeur lue × 2) | 35 |
| ADC solaire (non connecté sur toutes les révisions) | 36 |

## Fonctionnalités

- **Provisioning WiFi** : au premier démarrage (ou si les identifiants enregistrés ne fonctionnent plus), l'appareil ouvre un point d'accès WiFi `ESP32-Setup` avec un formulaire web (`http://192.168.4.1/`) pour saisir le SSID/mot de passe du réseau. Les identifiants sont sauvegardés en NVS et l'appareil redémarre pour s'y connecter.
- **Envoi / lecture / liste / suppression de SMS** via des commandes AT (mode texte). La liste et la lecture retournent du JSON structuré (id, statut, expéditeur, horodatage, texte), obtenu en interrogeant chaque emplacement SIM individuellement (`AT+CMGR`) plutôt qu'en parsant la réponse multi-lignes `AT+CMGL`.
- **Carnet de contacts** (5 maximum) : CRUD persisté en NVS, liste visible sur le tableau de bord.
- **Alerte SMS batterie/secteur** : toutes les minutes, si la tension batterie passe sous 3800 mV (et reste au-dessus de 3000 mV, pour ignorer l'absence de batterie), un SMS "Gateway sur batterie" est envoyé à tous les contacts. Au retour au-dessus de 3950 mV, un SMS "Gateway sur secteur" est envoyé. L'état est persisté en NVS pour ne pas renvoyer l'alerte après un redémarrage (la bascule secteur/batterie peut provoquer un reset).
- **Alimentation & énergie** : maintien de la carte sous tension sur batterie (sans USB), lecture des tensions batterie/solaire.
- **Tableau de bord web** intégré au firmware (`GET /`), affichant en direct les informations système, l'état du modem et les contacts.
- **API REST** (`/api/v1/*`) protégée par une clé API (`X-API-Key`) pour les actions sensibles, avec mDNS (`<hostname>.local`) pour la découverte sur le réseau local.
- **Statut détaillé du modem** : signal, enregistrement réseau, type de réseau (2G/3G/4G), SMSC, identité du modem (fabricant, modèle, révision, IMEI).

## Routes API

Toutes les routes commencent par `/api/v1`. Le détail complet avec exemples de requêtes est disponible dans [api.http](api.http).

### Publiques (aucune protection)

| Méthode | Route | Description |
|---|---|---|
| GET | `/` | Tableau de bord HTML embarqué |
| GET | `/system/info` | Infos système (chip, version IDF, version app, tensions batterie/solaire...) |
| GET | `/board/status` | État du modem (signal, réseau, SMS en attente, SMSC, identité...) |
| GET | `/contacts` | Liste des contacts enregistrés |

### Protégées (en-tête `X-API-Key` requis)

| Méthode | Route | Description |
|---|---|---|
| POST | `/sms/send` | Envoyer un SMS (`{"to": "...", "text": "..."}`) |
| GET | `/sms/list` | Lister les SMS stockés sur la SIM (tableau JSON structuré) |
| GET | `/sms/read?index=N` | Lire un SMS précis (objet JSON structuré) |
| DELETE | `/sms/delete?index=N` | Supprimer un SMS précis |
| POST | `/system/reboot` | Redémarrer l'appareil |
| POST | `/contacts` | Ajouter un contact (`{"name": "...", "phone": "..."}`, 5 max) |
| PUT | `/contacts?id=N` | Modifier un contact |
| DELETE | `/contacts?id=N` | Supprimer un contact |

## Configuration

Via `idf.py menuconfig` → **Example Configuration** :

- **mDNS Host Name** (`CONFIG_EXAMPLE_MDNS_HOST_NAME`, défaut `dashboard`)
- **Clé API REST** (`CONFIG_REST_API_KEY`, défaut `changeme`) — **à changer impérativement avant toute mise en production**

Un fichier `.env` (`API_KEY=...`, `TARGET_NUMBER=...`) est utilisé par [api.http](api.http) (extension REST Client) pour les tests, `API_KEY` doit correspondre à `CONFIG_REST_API_KEY`.

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
  board.c/.h      # Pilotage du modem SIMCom (AT commands), parsing des réponses
  power.c/.h      # Alimentation carte (BOARD_POWERON_PIN), lecture batterie/solaire (ADC) et alerte SMS batterie/secteur
  contacts.c/.h   # CRUD du carnet de contacts, persisté en NVS
  wifi_prov.c/.h  # Provisioning WiFi (SoftAP + portail de configuration)
  rest_server.c   # Serveur HTTP et endpoints /api/v1/*
partitions_example.csv  # Table de partitions (nvs, phy_init, factory)
api.http          # Requêtes de test (extension VS Code REST Client)
```
