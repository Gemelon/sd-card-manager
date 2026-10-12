<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Third-party notices

SD Card Manager's original source and documentation: **Copyright 2026
Thomas Stoll, Apache-2.0**, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Dependency licenses are not replaced by this license.

## Dependency provenance

| Dependency | Source | License / preserved text |
| --- | --- | --- |
| ESP-IDF 6.0.1 | https://github.com/espressif/esp-idf/tree/v6.0.1 | [Apache-2.0](third_party/licenses/esp-idf/LICENSE) for Espressif's original code; SDK third-party files retain their own terms |
| FatFs | ESP-IDF `components/fatfs/src/ff.c` | [Original license header](third_party/licenses/esp-idf/FatFs-NOTICE.txt), Copyright 2022 ChaN; custom permissive conditions, not Apache-2.0 |
| FreeRTOS-Kernel | `components/freertos/FreeRTOS-Kernel/LICENSE.md` | [MIT](third_party/licenses/esp-idf/FreeRTOS-LICENSE.md), Amazon.com and contributors |
| cJSON | https://components.espressif.com/components/espressif/cjson | [MIT](third_party/licenses/cjson/LICENSE), Dave Gamble and cJSON contributors; snapshot from 1.7.19~2 |
| Newlib / Picolibc | SDK `components/esp_libc/` | [Newlib](third_party/licenses/esp-idf/COPYING.NEWLIB), [Picolibc](third_party/licenses/esp-idf/COPYING.picolibc); multiple authors/terms |
| GCC runtime | Xtensa esp-15.2.0_20251204 | [GPL-3.0](third_party/licenses/toolchain/COPYING3) with [Runtime Library Exception](third_party/licenses/toolchain/COPYING.RUNTIME); [LGPL text](third_party/licenses/toolchain/COPYING3.LIB), [libgloss terms](third_party/licenses/toolchain/COPYING.LIBGLOSS) |
| OTA Suite | https://github.com/Gemelon/ota-suite | Apache-2.0, Copyright 2026 Thomas Stoll; its own NOTICE and THIRD_PARTY_NOTICES apply |
| Bootstrap 5.3.3, via OTA Suite | https://github.com/twbs/bootstrap/tree/v5.3.3 | MIT, Copyright 2011-2024 The Bootstrap Authors; complete license delivered by OTA Suite at `components/web_interface/assets/BOOTSTRAP-LICENSE` |

License snapshots are copied without changing copyright statements.
The consuming application's lockfile determines managed dependency versions.
OTA Suite documents the additional WLAN, TLS and web dependencies; preserve
those notices when including that package.

[TLSF's original BSD license block](third_party/licenses/esp-idf/TLSF-NOTICE.txt)
is preserved from https://github.com/mattconte/tlsf (`tlsf.h`).
[SOURCE-NOTICES.txt](third_party/licenses/esp-idf/SOURCE-NOTICES.txt) collects
unchanged source-level notices from the EBCTester reference build; it is a
conservative superset, not just SD code. Standard BSD, MIT and CC0 texts are
in [standard/](third_party/licenses/standard/). Additional SDK license snapshots
are supplied for the combined application and may not be needed by every consumer.

## Distribution

Keep this package's `LICENSE`, `NOTICE`, this document and
`third_party/licenses` with source and firmware redistributions. Include
the corresponding OTA Suite materials and Bootstrap license when using its UI.
For binary-only firmware releases, provide license texts as accompanying
documentation, not merely a URL.

Retain original MIT/BSD/FatFs notices and source headers, all applicable
Apache NOTICE files, and the runtime license exception. File-level terms
within SDK/libc source remain applicable. Mark changed Apache-covered files
and do not imply upstream endorsement. Review additional linked code whenever
the SDK, toolchain, hardware configuration or dependencies change.
