/* SPDX-License-Identifier: GPL-2.0-only */
#include "yukari/parcel.h"

#ifdef __KERNEL__
#include <linux/string.h>
typedef unsigned long yukari_address;
#else
#include <string.h>
typedef uintptr_t yukari_address;
#endif

struct name_view {
	size_t data;
	size_t units;
	size_t end;
};

struct entry_view {
	size_t start;
	size_t end;
	struct name_view name;
};

static yukari_u16 get16(const yukari_u8 *data)
{
	return (yukari_u16)((yukari_u16)data[0] |
			   ((yukari_u16)data[1] << 8));
}

static yukari_u32 get32(const yukari_u8 *data)
{
	return (yukari_u32)data[0] | ((yukari_u32)data[1] << 8) |
	       ((yukari_u32)data[2] << 16) | ((yukari_u32)data[3] << 24);
}

static void put32(yukari_u8 *data, yukari_u32 value)
{
	data[0] = (yukari_u8)value;
	data[1] = (yukari_u8)(value >> 8);
	data[2] = (yukari_u8)(value >> 16);
	data[3] = (yukari_u8)(value >> 24);
}

static unsigned int ascii_lower(unsigned int value)
{
	return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static enum yukari_parcel_status validate_tokens(const char *const *tokens,
						size_t count)
{
	size_t i, j;

	if (count > YUKARI_PARCEL_MAX_TOKENS)
		return YUKARI_PARCEL_LIMIT;
	if (count && !tokens)
		return YUKARI_PARCEL_INVALID;
	for (i = 0; i < count; ++i) {
		if (!tokens[i] || !tokens[i][0])
			return YUKARI_PARCEL_INVALID;
		for (j = 0; j <= YUKARI_PARCEL_MAX_TOKEN_BYTES; ++j) {
			unsigned int c = (unsigned char)tokens[i][j];

			if (!c)
				break;
			if (c > 0x7f)
				return YUKARI_PARCEL_INVALID;
		}
		if (j > YUKARI_PARCEL_MAX_TOKEN_BYTES)
			return YUKARI_PARCEL_LIMIT;
	}
	return YUKARI_PARCEL_OK;
}

static enum yukari_parcel_status read_name(const yukari_u8 *data,
			size_t limit, size_t start, struct name_view *view)
{
	yukari_u32 units;
	size_t bytes, padded, pos, i;

	if ((start & 3u) || start > limit || limit - start < 4u)
		return YUKARI_PARCEL_INVALID;
	units = get32(data + start);
	/* Null is not legal for IServiceManager's nonnullable names. */
	if (units & 0x80000000u)
		return YUKARI_PARCEL_INVALID;
	if (units > YUKARI_PARCEL_MAX_NAME_UNITS)
		return YUKARI_PARCEL_LIMIT;
	bytes = ((size_t)units + 1u) * 2u;
	padded = (bytes + 3u) & ~(size_t)3u;
	pos = start + 4u;
	if (padded > limit - pos)
		return YUKARI_PARCEL_INVALID;
	if (get16(data + pos + (size_t)units * 2u) != 0)
		return YUKARI_PARCEL_INVALID;
	for (i = 0; i < units; ++i) {
		yukari_u16 c = get16(data + pos + i * 2u);

		if (c >= 0xd800u && c <= 0xdbffu) {
			if (++i == units)
				return YUKARI_PARCEL_INVALID;
			c = get16(data + pos + i * 2u);
			if (c < 0xdc00u || c > 0xdfffu)
				return YUKARI_PARCEL_INVALID;
		} else if (c >= 0xdc00u && c <= 0xdfffu) {
			return YUKARI_PARCEL_INVALID;
		}
	}
	/* Android's Parcel writers zero the alignment bytes. */
	for (i = bytes; i < padded; ++i)
		if (data[pos + i])
			return YUKARI_PARCEL_INVALID;
	view->data = pos;
	view->units = units;
	view->end = pos + padded;
	return YUKARI_PARCEL_OK;
}

static int matches(const yukari_u8 *data, const struct name_view *name,
		   const char *const *tokens, size_t token_count,
		   unsigned int match_mode)
{
	size_t t, offset, i, length;

	for (t = 0; t < token_count; ++t) {
		length = strlen(tokens[t]); /* Validated and bounded first. */
		if (length > name->units ||
		    (match_mode == YUKARI_PARCEL_MATCH_EXACT && length != name->units))
			continue;
		for (offset = 0; offset <= name->units - length; ++offset) {
			if (match_mode == YUKARI_PARCEL_MATCH_PREFIX && offset != 0)
				break;
			for (i = 0; i < length; ++i)
				if (ascii_lower(get16(data + name->data +
						  (offset + i) * 2u)) !=
				    ascii_lower((unsigned char)tokens[t][i]))
					break;
			if (i == length)
				return 1;
			if (match_mode != YUKARI_PARCEL_MATCH_SUBSTRING)
				break;
		}
	}
	return 0;
}

static enum yukari_parcel_status read_entry(const yukari_u8 *data,
				 size_t limit, size_t start, int debug,
				 struct entry_view *entry)
{
	enum yukari_parcel_status status;
	size_t parcel_start, end;
	yukari_u32 parcel_size;

	entry->start = start;
	if (!debug) {
		status = read_name(data, limit, start, &entry->name);
		if (status != YUKARI_PARCEL_OK)
			return status;
		entry->end = entry->name.end;
		return YUKARI_PARCEL_OK;
	}
	if ((start & 3u) || start > limit || limit - start < 8u)
		return YUKARI_PARCEL_INVALID;
	if (get32(data + start) != 1u)
		return YUKARI_PARCEL_INVALID;
	parcel_start = start + 4u;
	parcel_size = get32(data + parcel_start);
	if (parcel_size & 0x80000000u)
		return YUKARI_PARCEL_INVALID;
	if (parcel_size > YUKARI_PARCEL_MAX_BYTES)
		return YUKARI_PARCEL_LIMIT;
	if (parcel_size < 4u || (parcel_size & 3u) ||
	    parcel_size > limit - parcel_start)
		return YUKARI_PARCEL_INVALID;
	end = parcel_start + parcel_size;
	status = read_name(data, end, parcel_start + 4u, &entry->name);
	if (status != YUKARI_PARCEL_OK)
		return status;
	if (end - entry->name.end < 4u) /* debugPid is mandatory. */
		return YUKARI_PARCEL_INVALID;
	entry->end = end;
	return YUKARI_PARCEL_OK;
}

static int partially_overlaps(const void *input, size_t input_size,
			      const void *output, size_t output_size)
{
	yukari_address a = (yukari_address)input;
	yukari_address b = (yukari_address)output;

	if (a == b)
		return 0;
	return a < b ? b - a < input_size : a - b < output_size;
}

static enum yukari_parcel_status scan_vector(
	const void *input, size_t input_size,
	const char *const *tokens, size_t token_count,
			struct yukari_parcel_result *result, int debug,
			unsigned int match_mode)
{
	const yukari_u8 *source = input;
	enum yukari_parcel_status status;
	struct yukari_parcel_result computed = { 8u, 0u, 0u };
	struct entry_view entry;
	yukari_u32 count, index;
	size_t cursor;

	if (!input || !result)
		return YUKARI_PARCEL_INVALID;
	if (input_size > YUKARI_PARCEL_MAX_BYTES)
		return YUKARI_PARCEL_LIMIT;
	if (input_size < 8u || (input_size & 3u))
		return YUKARI_PARCEL_INVALID;
	status = validate_tokens(tokens, token_count);
	if (status != YUKARI_PARCEL_OK)
		return status;
	if (match_mode > YUKARI_PARCEL_MATCH_EXACT)
		return YUKARI_PARCEL_UNSUPPORTED;
	/* No rich exception/fat StrictMode/work-source headers are accepted. */
	if (get32(source) != 0u)
		return YUKARI_PARCEL_UNSUPPORTED;
	count = get32(source + 4u);
	if (count & 0x80000000u)
		return YUKARI_PARCEL_INVALID;
	if (count > YUKARI_PARCEL_MAX_ENTRIES)
		return YUKARI_PARCEL_LIMIT;
	cursor = 8u;
	for (index = 0; index < count; ++index) {
		status = read_entry(source, input_size, cursor, debug, &entry);
		if (status != YUKARI_PARCEL_OK)
			return status;
		if (matches(source, &entry.name, tokens, token_count, match_mode)) {
			++computed.removed;
		} else {
			++computed.kept;
			computed.size += entry.end - entry.start;
		}
		cursor = entry.end;
	}
	if (cursor != input_size)
		return YUKARI_PARCEL_INVALID;
	*result = computed;
	return YUKARI_PARCEL_OK;
}

static enum yukari_parcel_status filter_vector(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count,
			struct yukari_parcel_result *result, int debug,
			unsigned int match_mode)
{
	const yukari_u8 *source = input;
	yukari_u8 *destination = output;
	enum yukari_parcel_status status;
	struct yukari_parcel_result computed;
	struct entry_view entry;
	yukari_u32 count, index;
	size_t cursor, write_cursor;

	if (!input || !output || !result)
		return YUKARI_PARCEL_INVALID;
	status = scan_vector(input, input_size, tokens, token_count, &computed,
				     debug, match_mode);
	if (status != YUKARI_PARCEL_OK)
		return status;
	if (capacity < computed.size)
		return YUKARI_PARCEL_NO_SPACE;
	if ((input == output && token_count != 0) ||
	    partially_overlaps(input, input_size, output, computed.size))
		return YUKARI_PARCEL_INVALID;
	/* Full validation precedes the first write, including in-place use. */
	count = get32(source + 4u);
	cursor = write_cursor = 8u;
	for (index = 0; index < count; ++index) {
		/* Stable input was checked above; compaction never reaches ahead. */
		(void)read_entry(source, input_size, cursor, debug, &entry);
		if (!matches(source, &entry.name, tokens, token_count, match_mode)) {
			memmove(destination + write_cursor, source + cursor,
				entry.end - cursor);
			write_cursor += entry.end - cursor;
		}
		cursor = entry.end;
	}
	put32(destination, 0u);
	put32(destination + 4u, computed.kept);
	*result = computed;
	return YUKARI_PARCEL_OK;
}

enum yukari_parcel_status yukari_parcel_filter_strings(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count,
	struct yukari_parcel_result *result)
{
	return filter_vector(input, input_size, output, capacity, tokens,
			     token_count, result, 0, YUKARI_PARCEL_MATCH_SUBSTRING);
}

enum yukari_parcel_status yukari_parcel_filter_debug_info(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count,
	struct yukari_parcel_result *result)
{
	return filter_vector(input, input_size, output, capacity, tokens,
			     token_count, result, 1, YUKARI_PARCEL_MATCH_SUBSTRING);
}

enum yukari_parcel_status yukari_parcel_filter_strings_mode(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count, unsigned int match_mode,
	struct yukari_parcel_result *result)
{
	return filter_vector(input, input_size, output, capacity, tokens,
			     token_count, result, 0, match_mode);
}

enum yukari_parcel_status yukari_parcel_filter_debug_info_mode(
	const void *input, size_t input_size, void *output, size_t capacity,
	const char *const *tokens, size_t token_count, unsigned int match_mode,
	struct yukari_parcel_result *result)
{
	return filter_vector(input, input_size, output, capacity, tokens,
			     token_count, result, 1, match_mode);
}

enum yukari_parcel_status yukari_parcel_validate_strings(
	const void *input, size_t input_size, struct yukari_parcel_result *result)
{
	return scan_vector(input, input_size, NULL, 0u, result, 0,
			  YUKARI_PARCEL_MATCH_SUBSTRING);
}

enum yukari_parcel_status yukari_parcel_validate_debug_info(
	const void *input, size_t input_size, struct yukari_parcel_result *result)
{
	return scan_vector(input, input_size, NULL, 0u, result, 1,
			  YUKARI_PARCEL_MATCH_SUBSTRING);
}

enum yukari_parcel_status yukari_parcel_match_utf16_le(
	const void *utf16_le, size_t units, const char *const *tokens,
	size_t token_count, unsigned int match_mode, int *matched)
{
	const yukari_u8 *data = utf16_le;
	struct name_view name = { 0u, units, units * 2u };
	enum yukari_parcel_status status;
	size_t i;

	if (!matched || (units != 0u && !data))
		return YUKARI_PARCEL_INVALID;
	if (units > YUKARI_PARCEL_MAX_NAME_UNITS)
		return YUKARI_PARCEL_LIMIT;
	status = validate_tokens(tokens, token_count);
	if (status != YUKARI_PARCEL_OK)
		return status;
	if (match_mode > YUKARI_PARCEL_MATCH_EXACT)
		return YUKARI_PARCEL_UNSUPPORTED;
	for (i = 0u; i < units; ++i) {
		yukari_u16 c = get16(data + i * 2u);

		if (c >= 0xd800u && c <= 0xdbffu) {
			if (++i == units)
				return YUKARI_PARCEL_INVALID;
			c = get16(data + i * 2u);
			if (c < 0xdc00u || c > 0xdfffu)
				return YUKARI_PARCEL_INVALID;
		} else if (c >= 0xdc00u && c <= 0xdfffu) {
			return YUKARI_PARCEL_INVALID;
		}
	}
	*matched = matches(data, &name, tokens, token_count, match_mode);
	return YUKARI_PARCEL_OK;
}

enum yukari_parcel_status yukari_parcel_match_string16(
	const void *input, size_t input_size, size_t offset,
	const char *const *tokens, size_t token_count, unsigned int match_mode,
	int *matched, size_t *end)
{
	const yukari_u8 *data = input;
	struct name_view name;
	enum yukari_parcel_status status;

	if (!input || !matched || !end)
		return YUKARI_PARCEL_INVALID;
	if (input_size > YUKARI_PARCEL_MAX_BYTES)
		return YUKARI_PARCEL_LIMIT;
	status = validate_tokens(tokens, token_count);
	if (status != YUKARI_PARCEL_OK)
		return status;
	if (match_mode > YUKARI_PARCEL_MATCH_EXACT)
		return YUKARI_PARCEL_UNSUPPORTED;
	status = read_name(data, input_size, offset, &name);
	if (status != YUKARI_PARCEL_OK)
		return status;
	*matched = matches(data, &name, tokens, token_count, match_mode);
	*end = name.end;
	return YUKARI_PARCEL_OK;
}

enum yukari_parcel_status yukari_parcel_make_null_binder(
	void *output, size_t capacity, unsigned int binder_pointer_size,
	size_t *output_size)
{
	yukari_u8 *data = output;
	size_t size;

	if (!output || !output_size)
		return YUKARI_PARCEL_INVALID;
	if (binder_pointer_size != 4u && binder_pointer_size != 8u)
		return YUKARI_PARCEL_UNSUPPORTED;
	size = 16u + 2u * binder_pointer_size;
	if (capacity < size)
		return YUKARI_PARCEL_NO_SPACE;
	memset(data, 0, size);
	/* The reply begins with a successful status, followed by the object. */
	put32(data, 0u);
	/* B_PACK_CHARS('s', 'b', '*', 0x85): BINDER_TYPE_BINDER. */
	put32(data + 4u, 0x73622a85u);
	/* flat_binder_object.flags: default SCHED_NORMAL priority (19). */
	put32(data + 8u, 19u);
	/* The stability representation follows the flat object. */
	put32(data + size - 4u, 0u);
	*output_size = size;
	return YUKARI_PARCEL_OK;
}
