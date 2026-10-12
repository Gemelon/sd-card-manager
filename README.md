# SD Card Manager

Reusable ESP-IDF component for an ESP32-S3 SD card in 4-bit SDMMC mode and an
authenticated Bootstrap 5 file-management tab for the OTA Suite web interface.

## Repository, dependencies and licensing

Clone `https://github.com/Gemelon/sd-card-manager.git`. This repository is
currently private. The component requires ESP-IDF and OTA Suite's
`web_interface` component, including its bundled `I18n` browser runtime.
EBCTester pins both packages as submodules; standalone consumers must supply
the compatible OTA Suite package through `EXTRA_COMPONENT_DIRS` too.

No license has yet been granted for this package's original source code.
ESP-IDF and managed `espressif/cjson` retain their upstream licenses
(Apache-2.0 for IDF's original code and MIT for cJSON). Bootstrap assets are
provided by OTA Suite, with the license included in that package.
Do not commit local `sdkconfig`, credentials, measurements or build outputs.

## Hardware defaults

| SD signal | GPIO |
| --- | ---: |
| CLK | 14 |
| CMD | 15 |
| D0 | 2 |
| D1 | 4 |
| D2 | 5 |
| D3 | 6 |
| Card detect | 7 (active high = card inserted) |

GPIOs and the SDMMC clock can be changed in menuconfig under **SD Card Manager**.
The active-high card-detect input is configured with an internal pull-up and
polled every 250 ms;
four consistent readings are required before accepting an insertion/removal.
The card is mounted only while GPIO 7 reads high; removal disables file actions
and unmounts the filesystem before retrying. The SD tab refreshes card state
automatically while open, so a card inserted after startup is mounted and shown
without a page reload. The tab displays the configured GPIO and its live HIGH/LOW
level to help verify the detect wire. GPIO internal pull-ups are weak; if the
line does not reach a stable 3.3 V logic HIGH, add an external pull-up (for
example 4.7 kOhm to 3.3 V) and ensure the card-detect output is not driven by 5 V.
The default clock is 10 MHz to favor reliability on jumper-wire setups; lower it
further if the signal wiring is long or file operations remain intermittent.
Use suitable external pull-ups on the SDMMC bus; internal pull-ups are enabled
only as a debug fallback and do not replace board-level pull-ups.

## Filesystem and data safety

The component uses the ESP-IDF FatFs integration and expects FAT32 media. ESP-IDF
6.0.1 ships with exFAT disabled in its bundled FatFs configuration, so exFAT is
not supported by this component. A missing, unformatted, or unsupported card is
reported in the web interface; the component never formats a card automatically.
Format or back up the card separately before use.

## Add to an application

Add this package's `components` directory to `EXTRA_COMPONENT_DIRS`, require
`sd_card_manager` from the application component, and call
`sd_card_manager_init()` before `device_app_start()`. The initializer registers
the **SD-Karte** tab with the OTA Suite shared Bootstrap UI and starts a
non-blocking card retry task. A card that is unavailable does not prevent the
firmware or OTA services from starting.

Failure to calculate free space does not mark the card unmounted. The UI reports
unknown free capacity and uploads proceed to the filesystem, which reports an
actual no-space error if it cannot accept the data.

The configured mount point defaults to `/sdcard`. HTTP operations are protected
by the shared Basic authentication and CSRF token. The browser supports folder
listing, downloads, uploads, empty-file and folder creation, rename/move, and
deletion. Non-empty folders cannot be deleted, preventing accidental recursive
data loss.

Other components that access the mounted filesystem should pair
`sd_card_manager_storage_acquire()` with `sd_card_manager_storage_release()`.
The shared lock prevents file-manager actions or hot-removal handling from
unmounting the card during an operation; acquire returns an error while the
card is unavailable.
