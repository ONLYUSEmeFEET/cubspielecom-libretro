#ifndef CUBSPIELE_ZIP_BIOS_H
#define CUBSPIELE_ZIP_BIOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Loads the 4KB (4096-byte) internal BIOS and 256KB (262144-byte) external BIOS
 * from a ZIP archive (e.g. RetroArch's system/gamecom.zip).
 * Matches entries by uncompressed size (4096 / 262144), supporting both MAME
 * (internal.bin/external.bin) and No-Intro ([BIOS] Tiger Game.com ... .tgc) names.
 */
bool zip_load_gamecom_bios(const char *zip_path,
                           uint8_t *out_internal_4k,
                           uint8_t *out_external_256k);

#ifdef __cplusplus
}
#endif

#endif /* CUBSPIELE_ZIP_BIOS_H */
