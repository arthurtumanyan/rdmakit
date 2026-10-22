/*
 * sha256.h - SHA-256 (FIPS 180-4) for password hashes of the access control lists.
 *
 * The builds without TLS have no cryptographic library, so the hash is implemented here. It is used only
 * on authentication and when ACL files are loaded, not on the request path.
 */
#ifndef AKV_SHA256_H
#define AKV_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define AKV_SHA256_LEN 32u

/* Writes the SHA-256 digest of data[0, n) to out. */
void akv_sha256(const void *data, size_t n, uint8_t out[AKV_SHA256_LEN]);

#endif
