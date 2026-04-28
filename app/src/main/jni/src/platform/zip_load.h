/*
 * zip_load.h
 * Extract the first Atari ROM entry from a ZIP archive.
 * Reads the central directory (not local headers) so bit-3 data-descriptor
 * ZIPs work correctly.  Supports METHOD_STORED (0) and METHOD_DEFLATE (8).
 */
#ifndef ZIP_LOAD_H
#define ZIP_LOAD_H

/*
 * Extract the first .a26/.a78/.bin entry from the ZIP at zip_path.
 *
 * On success:
 *   *data_out   — malloc'd buffer with uncompressed ROM data (caller must free)
 *   *size_out   — uncompressed size in bytes
 *   *mtype_out  — MACHINE_2600 or MACHINE_7800
 *   return value 0
 *
 * On failure: returns -1 (no ROM entry found, bad ZIP, unsupported method, etc.)
 */
int zip_extract_rom(const char *zip_path,
                    unsigned char **data_out,
                    unsigned long  *size_out,
                    int            *mtype_out);

/*
 * Return the machine type (MACHINE_2600 / MACHINE_7800) of the first ROM
 * entry in the ZIP without extracting its data.
 * Returns -1 if no ROM entry is found or the file cannot be read.
 */
int zip_probe_machine_type(const char *zip_path);

#endif /* ZIP_LOAD_H */
