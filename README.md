# SD Card Manager

Reusable ESP-IDF component for an ESP32-S3 SD card in 4-bit SDMMC mode and an
authenticated Bootstrap 5 file-management tab for the OTA Suite web interface.

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
The card-detect input is configured with a pull-down and polled once per second.
The card is mounted only while GPIO 7 reads high; removal disables file actions
and unmounts the filesystem before retrying.
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
