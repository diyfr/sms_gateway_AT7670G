# A7670G SMS Gateway

Passerelle SMS basée sur ESP32 + modem cellulaire A7670G (compatible SIM7600), exposant une API REST pour envoyer, lire, lister et supprimer des SMS, consulter l'état du modem/de la carte (dont batterie/solaire), gérer un carnet de contacts et piloter une centrale d'alarme Meian (IP + commandes SMS).

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
- **Carnet de contacts** (5 maximum) : CRUD persisté en NVS, liste visible sur le tableau de bord. Chaque contact peut avoir un code PIN optionnel (4 à 6 chiffres) utilisé pour autoriser les commandes SMS de l'alarme Meian ; ce code n'est renvoyé par `GET /contacts` que si une clé API valide est fournie (masqué pour les clients anonymes qui consultent le tableau de bord).
- **Intégration alarme Meian [DRAFT]** (`meian.c`/`.h`) : configuration (activation + adresse IP de la centrale) via `/api/meian`, persistée en NVS.
  - Lorsque activée, une tâche interroge périodiquement la centrale (TCP, protocole propriétaire chiffré XOR) et envoie un SMS à tous les contacts disposant d'un code PIN dès que l'état (désarmée / armée totale / armée périmétrique / déclenchée) change.
  - Une seconde tâche surveille les **SMS non lus** (`AT+CMGL="REC UNREAD"`, sans en modifier le statut) à la recherche de commandes au format `#PWD<pin>#<CMD>` (`CMD` = `ARM`, `DISARM` ou `CHECK`) ; la commande n'est exécutée que si l'expéditeur correspond à un contact dont le code PIN correspond. Le SMS de commande est ensuite marqué comme lu (jamais supprimé) pour éviter qu'il soit rejoué, sans toucher aux autres SMS.
- **Alerte SMS batterie/secteur** : toutes les minutes, la tension batterie est classée en 3 niveaux, et un SMS est envoyé à tous les contacts uniquement lors d'un changement de niveau :
  - ≥ 4000 mV → **secteur** ("Gateway sur secteur")
  - [3700, 3900) mV → **sur batterie** ("Gateway sur batterie")
  - [3000, 3700) mV → **batterie critique** ("Gateway batterie niveau critique")
  - < 3000 mV → ignoré (pas de batterie connectée / lecture non fiable)
  - [3900, 4000) mV → zone tampon (hystérésis), aucun changement d'état

  Le niveau est persisté en NVS pour ne pas renvoyer l'alerte après un redémarrage (la bascule secteur/batterie peut provoquer un reset).
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
| GET | `/contacts` | Liste des contacts enregistrés (champ `pin` inclus uniquement si `X-API-Key` valide) |

### Protégées (en-tête `X-API-Key` requis)

| Méthode | Route | Description |
|---|---|---|
| POST | `/sms/send` | Envoyer un SMS (`{"to": "...", "text": "..."}`) |
| GET | `/sms/list` | Lister les SMS stockés sur la SIM (tableau JSON structuré) |
| GET | `/sms/read?index=N` | Lire un SMS précis (objet JSON structuré) |
| DELETE | `/sms/delete?index=N` | Supprimer un SMS précis |
| POST | `/system/reboot` | Redémarrer l'appareil |
| POST | `/contacts` | Ajouter un contact (`{"name": "...", "phone": "...", "pin": "1234"}`, `pin` optionnel, 5 max) |
| PUT | `/contacts?id=N` | Modifier un contact (`pin: null` pour retirer le code) |
| DELETE | `/contacts?id=N` | Supprimer un contact |

En dehors de `/api/v1`, deux routes protégées pilotent la configuration Meian :

| Méthode | Route | Description |
|---|---|---|
| GET | `/api/meian` | Configuration courante (`{"enabled": bool, "ip": string\|null}`) |
| POST | `/api/meian` | Définit `enabled`/`ip` (`ip` obligatoire si `enabled=true`) |

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
  main.c          # Point d'entrée, init NVS/WiFi/mDNS, démarrage du serveur REST et des tâches Meian
  board.c/.h      # Pilotage du modem SIMCom (AT commands), parsing des réponses
  power.c/.h      # Alimentation carte (BOARD_POWERON_PIN), lecture batterie/solaire (ADC) et alerte SMS batterie/secteur
  contacts.c/.h   # CRUD du carnet de contacts (dont code PIN optionnel), persisté en NVS
  meian.c/.h      # Configuration + pilotage de la centrale d'alarme Meian, commandes SMS, notifications de changement d'état
  wifi_prov.c/.h  # Provisioning WiFi (SoftAP + portail de configuration)
  rest_server.c   # Serveur HTTP et endpoints /api/v1/* et /api/meian
partitions_example.csv  # Table de partitions (nvs, phy_init, factory)
api.http          # Requêtes de test (extension VS Code REST Client)
```


## Utilisation [api.http](api.http)
A utiliser avec le plugin [Rest-Client](https://marketplace.visualstudio.com/items?itemName=humao.rest-client)  

Définissez vos variables d'environnement dans un fichier `.env`  
```config
API_KEY=XXXXXXXXXXXX
TARGET_NUMBER=+33651000000
MEIAN_HOST=192.168.XXX.XXX
HOST=192.168.XXX.XXX
```