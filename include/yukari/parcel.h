/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef YUKARI_PARCEL_H
#define YUKARI_PARCEL_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 yukari_u8;
typedef u16 yukari_u16;
typedef u32 yukari_u32;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t yukari_u8;
typedef uint16_t yukari_u16;
typedef uint32_t yukari_u32;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit work limits, independent of Binder's own transaction limits. */
#define YUKARI_PARCEL_MAX_BYTES (1024u * 1024u)
#define YUKARI_PARCEL_MAX_ENTRIES 8192u
#define YUKARI_PARCEL_MAX_NAME_UNITS 4096u
#define YUKARI_PARCEL_MAX_TOKENS 32u
#define YUKARI_PARCEL_MAX_TOKEN_BYTES 64u

enum yukari_parcel_status {
	YUKARI_PARCEL_OK = 0,
	YUKARI_PARCEL_INVALID,
	YUKARI_PARCEL_LIMIT,
	YUKARI_PARCEL_NO_SPACE,
	YUKARI_PARCEL_UNSUPPORTED
};

struct yukari_parcel_result {
	size_t size;
	yukari_u32 kept;
	yukari_u32 removed;
};

/*
 * Android 14, little-endian, kernel-IPC AIDL reply data only. The caller must
 * independently verify the transaction method, successful transport status,
 * and an empty Binder object-offset table before filtering either vector.
 *
 * Tokens are nonempty NUL-terminated literal ASCII strings, matched as
 * case-insensitive substrings of validated UTF-16 names. token_count == 0
 * copies a validated reply without filtering. No allocation is performed.
 * Input/output must be disjoint when filtering is requested; with zero tokens
 * an exact in-place validated copy is allowed. Partial overlap is rejected.
 * On every error output and result are untouched. On success bytes beyond
 * result->size are unspecified, so the caller must publish only that length.
 * Input and tokens must remain stable for the duration of the call.
 */
enum yukari_parcel_status yukari_parcel_filter_strings(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count,
	struct yukari_parcel_result *result);

/*
 * Each ServiceDebugInfo has a non-null int32 marker followed by an AIDL
 * size-prefixed parcelable: String16 name, int32 debugPid, optional unknown
 * trailing fields. Kept parcelables, including those fields, stay byte exact.
 */
enum yukari_parcel_status yukari_parcel_filter_debug_info(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count,
	struct yukari_parcel_result *result);

/*
 * Produce a successful getService/checkService reply with a null Binder.
 * binder_pointer_size is the Binder ABI pointer width (4 or 8), which is not
 * necessarily the client C pointer width. Android 14's stability int32 is
 * included. The null flat object is intentionally omitted from Binder's object
 * offset table (Parcel::writeObject(..., false)); transport integration owns
 * that empty table and any reference operations. Output/size unchanged on
 * error.
 */
enum yukari_parcel_status yukari_parcel_make_null_binder(
	void *output, size_t capacity, unsigned int binder_pointer_size,
	size_t *output_size);

#ifdef __cplusplus
}
#endif
#endif
