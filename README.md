<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# SD Card Manager

**Eine SD-Karte im ESP32-Projekt nutzen und ihre Dateien bequem im Browser
verwalten – ohne die Speicheranbindung jedes Mal neu zu schreiben.**

Dieses Paket entstand für [EBCTester](https://github.com/Gemelon/EBCTester):
Lange Batterietests sollen ihre Messdaten auch außerhalb des Heim-WLANs
speichern können. Dafür braucht es lokalen Speicher und eine Oberfläche,
über die Daten und Sprachdateien erreichbar bleiben.

Die Kartenanbindung und Dateiverwaltung sind als eigene Komponenten ausgelagert,
damit sie auch in anderen ESP-IDF-Projekten verwendet werden können.

## Funktionen

- FAT32-SD-Karten über **4-bit SDMMC oder SPI**.
- Kartenwechsel-Erkennung, Mount/Unmount und nichtblockierende Wiederholungen.
- Dateiverwaltung im lokalen Webinterface: Ordner ansehen, Dateien hoch- und
  herunterladen, anlegen, umbenennen, verschieben und löschen.
- Gemeinsame Speichersperre für Anwendungen wie SQLite und SD-Sprachdateien.
- Kein automatisches Formatieren und kein rekursives Löschen gefüllter Ordner.
- Nicht verfügbare Karten verhindern nicht den Start der restlichen Anwendung.

## Architektur und Abhängigkeiten

| Repository | Aufgabe |
| --- | --- |
| [EBCTester](https://github.com/Gemelon/EBCTester) | Batterietests, Datenbank und Aufzeichnung |
| [ota-suite](https://github.com/Gemelon/ota-suite) | WLAN und gemeinsames authentifiziertes Webinterface |
| **sd-card-manager** | Speicheranbindung und Erweiterung dieses Webinterfaces |

Das Paket enthält die Komponente `components/sd_card_manager`.
Es setzt das `web_interface` der OTA Suite einschließlich `I18n`-Laufzeit
voraus, aber keinen EBCTester-Code. Die Anwendung bindet beide Komponentenpakete
über `EXTRA_COMPONENT_DIRS` ein; EBCTester hält ihre Stände als Submodule fest.

## Selbst verwenden und kompilieren

```powershell
git clone https://github.com/Gemelon/sd-card-manager.git
git clone https://github.com/Gemelon/ota-suite.git
```

Beide Repositories sind derzeit privat; ein berechtigtes Konto ist nötig.
Verwende VS Code mit ESP-IDF-Erweiterung und eine ESP-IDF-Anwendung.
Die Kombination ist mit **ESP-IDF 6.0.1 / ESP32-S3** getestet.
Dieses Paket allein ist keine flashbare Anwendung.

1. Beide `components`-Verzeichnisse in `EXTRA_COMPONENT_DIRS` aufnehmen.
2. `sd_card_manager` und `device_app` als Anwendungsabhängigkeiten angeben.
3. `sd_card_manager_init()` vor `device_app_start()` aufrufen.
4. Unter **menuconfig → SD Card Manager** Busmodus, Pins, Takt, Mountpunkt
   und Kartenerkennung an die Hardware anpassen.
5. Die eigene Anwendung über **ESP-IDF: Build your project** bauen.

Die Initialisierung registriert den SD-Tab und startet die Kartenprüfung.
HTTP-Aufrufe nutzen Anmeldung und CSRF-Schutz der OTA Suite.
Ein SD-Fehler wird gemeldet; die Anwendung kann trotzdem ihr Webinterface starten.

## Hardwarevorgaben

| Signal | SDMMC-GPIO |
| --- | ---: |
| CLK | 14 |
| CMD | 15 |
| D0 / D1 / D2 / D3 | 2 / 4 / 5 / 6 |
| Kartenerkennung | 7, aktiv HIGH |

SPI-Defaults: MISO 2, MOSI 15, CS 6, CLK 14; Kartenerkennung ebenfalls GPIO 7.
Beide Busse starten mit 10 MHz. Lange Leitungen können einen niedrigeren
Takt erfordern. Geeignete externe Pull-ups verwenden; interne Pull-ups sind
kein Ersatz für korrekte Hardware.

Die Kartenerkennung wird alle 250 ms geprüft; vier übereinstimmende Werte
bestätigen Einstecken oder Entfernen. Bei aktiv-HIGH-Erkennung ist ein stabiler
3,3-V-Pegel erforderlich. ESP32-Pins niemals mit 5 V ansteuern.
Der Tab zeigt GPIO und Pegel zur Diagnose.

## Dateisystem und Datenzugriff

FAT32 verwenden. exFAT ist in der verwendeten ESP-IDF-FatFs-Konfiguration
deaktiviert und wird nicht unterstützt. Karte separat formatieren und wichtige
Daten vorher sichern. Der Standard-Mountpunkt ist `/sdcard`.

Andere Komponenten müssen Zugriffe mit
`sd_card_manager_storage_acquire()` / `sd_card_manager_storage_release()`
paaren. Die Sperre verhindert konkurrierende Dateiverwaltung oder Unmount
während eines geschützten Zugriffs. Ohne verfügbare Karte schlägt Acquire
explizit fehl. Eine eigene Datenbank ist nicht Bestandteil dieses Pakets.

Fehler bei der Ermittlung freien Speichers markieren die Karte nicht als
unmounted. Die UI zeigt unbekannte Kapazität; Uploads erhalten tatsächliche
Dateisystemfehler, etwa bei einer vollen Karte.

Nichtleere Ordner können nicht gelöscht werden. Kartenentnahme und Stromausfall
können trotzdem Daten beschädigen; Speichersperren ersetzen keine Backups.
Die lokale HTTP-Oberfläche nur in einem vertrauenswürdigen Netz oder geschützten
AP nutzen, nicht öffentlich ins Internet stellen.

## Tests und Beiträge

Die Integration wird mit dem konsumierenden Projekt getestet; EBCTester hat
Hosttests für Datenbankschema und lokalisierte UI. Nach Änderungen SDMMC und
SPI, Kartenwechsel, Dateiaktionen, volle Karten und konkurrierende Speicherzugriffe
auf Hardware prüfen. Dieses Paket hat keinen eigenen Host-Test-Runner.

Fehlerberichte mit Board, Verdrahtung, SDK-Version und bereinigten Logs sind
willkommen. Lokale Zugangsdaten, Messdaten, `sdkconfig` und Build-Dateien nicht
committen. Bei Weitergabe von Firmware auch die Lizenzunterlagen beilegen.

## Lizenz und Urheber

**Copyright 2026 Thomas Stoll.** Ursprünglicher Code und Dokumentation stehen
unter **[Apache-2.0](LICENSE)**. Siehe [NOTICE](NOTICE).
Die Lizenz erlaubt auch gewerbliche Nutzung, Änderungen und Verkauf unter
ihren Bedingungen, ohne Gewährleistung. Geänderte Dateien kennzeichnen und
bestehende Hinweise erhalten. Formate ohne Kommentare sind in
[.reuse/dep5](.reuse/dep5) zugeordnet. Fremder Code behält seine eigenen Lizenzen.

## Drittanbieter und ihre Lizenzen

| Bestandteil | Urheber / Herkunft | Lizenz und Hinweise |
| --- | --- | --- |
| ESP-IDF 6.0.1 und SD-Treiber | Espressif Systems und weitere Beitragende | Apache-2.0 für den ursprünglichen Espressif-Code – [SDK-Lizenz](third_party/licenses/esp-idf/LICENSE) |
| FatFs | ChaN | Eigene permissive FatFs-Bedingungen – [Originalhinweis](third_party/licenses/esp-idf/FatFs-NOTICE.txt) |
| cJSON | Dave Gamble und cJSON contributors | MIT – [Originaltext](third_party/licenses/cjson/LICENSE) |
| FreeRTOS-Kernel | Amazon.com und Beitragende | MIT – [Originaltext](third_party/licenses/esp-idf/FreeRTOS-LICENSE.md) |
| C-Laufzeit: Newlib / Picolibc | Verschiedene Autoren | Eigene Bedingungen – [Newlib](third_party/licenses/esp-idf/COPYING.NEWLIB), [Picolibc](third_party/licenses/esp-idf/COPYING.picolibc) |
| GCC-Laufzeit | Free Software Foundation und Beitragende | GPL-3.0 mit GCC Runtime Library Exception 3.1 – [GPL](third_party/licenses/toolchain/COPYING3), [Ausnahme](third_party/licenses/toolchain/COPYING.RUNTIME) |
| OTA Suite | Thomas Stoll | Apache-2.0; [Repository und Drittanbieterübersicht](https://github.com/Gemelon/ota-suite#drittanbieter-und-ihre-lizenzen) |
| Bootstrap 5.3.3, über OTA Suite | The Bootstrap Authors | MIT; vollständiger Text unter `components/web_interface/assets/BOOTSTRAP-LICENSE` im OTA-Suite-Paket |
| TLSF | Matthew Conte | BSD-3-Clause – [Originalhinweis](third_party/licenses/esp-idf/TLSF-NOTICE.txt) |
| Weitere SDK-Bestandteile der Gesamtanwendung | Urheber laut ursprünglichen Headern | [Referenzbuild-Hinweise](third_party/licenses/esp-idf/SOURCE-NOTICES.txt) und [Standardtexte](third_party/licenses/standard/); konservativer Umfang, nicht alles wird vom SD-Paket benötigt |

WLAN-, TLS- und Web-Abhängigkeiten der konsumierenden Anwendung sind zusätzlich
in OTA Suite dokumentiert. Die Versionsauflösung erfolgt über den Lockfile
dieser Anwendung. Siehe [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) und
[third_party/licenses/](third_party/licenses/) für Originaltexte und Herkunft.
Die Projektlizenz ersetzt keine Drittanbieterlizenz; bei neuen Komponenten
oder SDK-Versionen müssen Hinweise erneut geprüft werden.
