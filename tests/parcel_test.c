/* SPDX-License-Identifier: GPL-2.0-only */
#include "yukari/parcel.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static unsigned int get32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static size_t put_name(unsigned char *p, const unsigned short *name, size_t n)
{
	size_t i, bytes = (n + 1u) * 2u, padded = (bytes + 3u) & ~3u;

	put32(p, (unsigned int)n);
	for (i = 0; i < n; ++i) {
		p[4u + i * 2u] = (unsigned char)name[i];
		p[5u + i * 2u] = (unsigned char)(name[i] >> 8);
	}
	memset(p + 4u + n * 2u, 0, padded - n * 2u);
	return 4u + padded;
}

static size_t make_strings(unsigned char *p)
{
	static const unsigned short a[] = {'a','n','d','r','o','i','d','.','o','k'};
	static const unsigned short b[] = {'V','e','n','d','o','r','.','L','I','N','E','A','G','E'};
	size_t pos = 8u;

	put32(p, 0u);
	put32(p + 4u, 3u);
	pos += put_name(p + pos, a, sizeof(a) / sizeof(a[0]));
	pos += put_name(p + pos, b, sizeof(b) / sizeof(b[0]));
	/* A valid empty String16 entry exercises zero-length and alignment. */
	pos += put_name(p + pos, NULL, 0u);
	return pos;
}

static size_t make_debug(unsigned char *p)
{
	static const unsigned short a[] = {'a','n','d','r','o','i','d','.','o','k'};
	static const unsigned short b[] = {'x','.','L','i','N','e','A','g','e'};
	size_t pos = 8u, begin, payload;

	put32(p, 0u);
	put32(p + 4u, 2u);
	put32(p + pos, 1u); /* non-null parcelable marker */
	begin = pos + 4u;
	put32(p + begin, 0u); /* size backpatched below */
	payload = put_name(p + begin + 4u, a, sizeof(a) / sizeof(a[0]));
	put32(p + begin + 4u + payload, 1234u);
	/* Unknown field is retained byte-for-byte by the filter. */
	put32(p + begin + 8u + payload, 0xdecafbadu);
	put32(p + begin, (unsigned int)(12u + payload));
	pos = begin + 12u + payload;
	put32(p + pos, 1u);
	begin = pos + 4u;
	put32(p + begin, 0u);
	payload = put_name(p + begin + 4u, b, sizeof(b) / sizeof(b[0]));
	put32(p + begin + 4u + payload, 4321u);
	put32(p + begin + 8u + payload, 0xfeedbeefu);
	put32(p + begin, (unsigned int)(12u + payload));
	return begin + 12u + payload;
}

static void test_strings(void)
{
	unsigned char input[256] = {0}, output[256] = {0};
	struct yukari_parcel_result result = {0};
	const char *tokens[] = {"lineage"};
	size_t size = make_strings(input);

	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_OK);
	assert(result.kept == 2u && result.removed == 1u);
	assert(result.size == 8u + 28u + 8u);
	assert(output[0] == 0 && output[4] == 2);
	/* No token means a validated byte-exact copy. */
	memset(output, 0xa5, sizeof(output));
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   NULL, 0u, &result) == YUKARI_PARCEL_OK);
	assert(result.size == size && memcmp(input, output, size) == 0);
	/* A non-ASCII name remains valid and does not match ASCII tokens. */
	assert(output[0] == 0);
	/* Filtering into the source buffer is rejected to prevent alias loss. */
	memset(input + size, 0, sizeof(input) - size);
	assert(yukari_parcel_filter_strings(input, size, input, sizeof(input),
					   tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	assert(yukari_parcel_filter_strings(input, size, input, sizeof(input),
					   NULL, 0u, &result) == YUKARI_PARCEL_OK);
}

static void test_unicode_and_alignment(void)
{
	unsigned char input[256] = {0}, output[256], before[256];
	const unsigned short unicode[] = {0x4e16u, 0x754cu, 0xd83du, 0xde80u};
	const char *tokens[] = {"lineage"};
	struct yukari_parcel_result result = {0};
	size_t size;

	put32(input, 0u);
	put32(input + 4u, 1u);
	size = 8u + put_name(input + 8u, unicode, 4u);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_OK);
	assert(result.kept == 1u && result.removed == 0u);
	assert(memcmp(input, output, size) == 0);
	memcpy(before, output, sizeof(before));
	input[8u + 4u + 2u * 4u + 2u] = 0x01; /* nonzero String16 pad */
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	assert(memcmp(before, output, sizeof(before)) == 0);
	/* Isolated surrogate is not a valid Android String16 value. */
	input[8u + 4u + 2u * 4u + 2u] = 0;
	input[8u + 4u + 2u * 3u] = 0x41;
	input[8u + 4u + 2u * 3u + 1u] = 0;
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
}

static void test_empty_all_hidden_and_tokens(void)
{
	unsigned char input[256] = {0}, output[256], before[256];
	struct yukari_parcel_result result = {0};
	const char *all[] = {"android", "lineage"};
	const char *empty[] = {""};
	const char *non_ascii[] = {"l\xC3\xAD"};
	const char *absent[] = {NULL};
	char long_token[YUKARI_PARCEL_MAX_TOKEN_BYTES + 2u];
	const char *too_long[] = {long_token};
	size_t size;

	/* Both empty vectors use the same success-status/count prefix. */
	assert(yukari_parcel_filter_strings(input, 8u, output, sizeof(output),
					   all, 2u, &result) == YUKARI_PARCEL_OK);
	assert(result.size == 8u && !result.kept && !result.removed);
	assert(yukari_parcel_filter_debug_info(input, 8u, output, sizeof(output),
					      all, 2u, &result) == YUKARI_PARCEL_OK);
	size = make_debug(input);
	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
					      all, 2u, &result) == YUKARI_PARCEL_OK);
	assert(result.size == 8u && !result.kept && result.removed == 2u);
	size = make_strings(input);
	memset(output, 0xc3, sizeof(output));
	memcpy(before, output, sizeof(output));
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   empty, 1u, &result) == YUKARI_PARCEL_INVALID);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   non_ascii, 1u, &result) == YUKARI_PARCEL_INVALID);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   absent, 1u, &result) == YUKARI_PARCEL_INVALID);
	memset(long_token, 'a', sizeof(long_token) - 1u);
	long_token[sizeof(long_token) - 1u] = 0;
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   too_long, 1u, &result) == YUKARI_PARCEL_LIMIT);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   all, YUKARI_PARCEL_MAX_TOKENS + 1u, &result) ==
	       YUKARI_PARCEL_LIMIT);
	assert(memcmp(output, before, sizeof(output)) == 0);
}

static void test_every_truncation(void)
{
	unsigned char input[256] = {0}, output[256], before[256];
	const char *tokens[] = {"lineage"};
	struct yukari_parcel_result result, untouched = {0};
	size_t size, n;
	int debug;

	untouched.size = 111u;
	untouched.kept = 222u;
	untouched.removed = 333u;
	memset(output, 0xd3, sizeof(output));
	memcpy(before, output, sizeof(output));
	for (debug = 0; debug != 2; ++debug) {
		size = debug ? make_debug(input) : make_strings(input);
		for (n = 0; n < size; ++n) {
			enum yukari_parcel_status status;

			result = untouched;
			status = debug ? yukari_parcel_filter_debug_info(
				input, n, output, sizeof(output), tokens, 1u, &result) :
				yukari_parcel_filter_strings(input, n, output,
					sizeof(output), tokens, 1u, &result);
			assert(status != YUKARI_PARCEL_OK);
			assert(memcmp(output, before, sizeof(output)) == 0);
			assert(result.size == untouched.size &&
			       result.kept == untouched.kept &&
			       result.removed == untouched.removed);
		}
	}
}

static void test_debug(void)
{
	unsigned char input[256] = {0}, output[256] = {0};
	struct yukari_parcel_result result = {0};
	const char *tokens[] = {"LINEAGE"};
	size_t size = make_debug(input);
	unsigned char expected[256] = {0};

	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_OK);
	assert(result.kept == 1u && result.removed == 1u);
	memcpy(expected, input, 8u);
	put32(expected + 4u, 1u);
	memcpy(expected + 8u, input + 8u, 4u + get32(input + 12u));
	assert(result.size == 8u + 4u + get32(input + 12u));
	assert(memcmp(output, expected, result.size) == 0);
	/* The retained parcelable's unknown field survives exactly. */
	assert(get32(output + result.size - 4u) == 0xdecafbadu);
	/* A size that runs beyond the input is rejected without output writes. */
	memset(output, 0xa7, sizeof(output));
	memcpy(expected, output, sizeof(output));
	put32(input + 12u, 0x7ffffffcu);
	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_LIMIT);
	assert(memcmp(output, expected, sizeof(output)) == 0);
}

static void test_invalid_is_unchanged(void)
{
	unsigned char input[256] = {0}, output[256], before[256];
	struct yukari_parcel_result result = {7u, 8u, 9u};
	const char *token[] = {"lineage"};
	size_t size = make_strings(input);

	memset(output, 0xcc, sizeof(output));
	memcpy(before, output, sizeof(output));
	put32(input + 8u, 5000u); /* Over the bounded String16 unit limit. */
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   token, 1u, &result) == YUKARI_PARCEL_LIMIT);
	assert(memcmp(output, before, sizeof(output)) == 0);
	assert(result.size == 7u && result.kept == 8u && result.removed == 9u);
	/* Truncation and malformed status are rejected before writing. */
	size = make_strings(input);
	assert(yukari_parcel_filter_strings(input, size - 1u, output, sizeof(output),
					   token, 1u, &result) == YUKARI_PARCEL_INVALID);
	put32(input, 1u);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   token, 1u, &result) == YUKARI_PARCEL_UNSUPPORTED);
	size = make_strings(input);
	memset(output, 0x5a, sizeof(output));
	memcpy(before, output, sizeof(output));
	assert(yukari_parcel_filter_strings(input, size, output, 8u,
					   NULL, 0u, &result) == YUKARI_PARCEL_NO_SPACE);
	assert(memcmp(output, before, sizeof(output)) == 0);
}

static void test_limits_and_null_binder(void)
{
	unsigned char output[64], before[64];
	size_t size = 0;
	const char *bad_token[] = {"l\xC3\xAD"};

	assert(yukari_parcel_make_null_binder(output, sizeof(output), 8u, &size) ==
	       YUKARI_PARCEL_OK);
	assert(size == 32u);
	assert(output[0] == 0 && output[4] == 0x85 && output[5] == 0x2a &&
	       output[6] == 0x62 && output[7] == 0x73 && output[8] == 19);
	assert(output[28] == 0 && output[29] == 0 && output[30] == 0 && output[31] == 0);
	assert(yukari_parcel_make_null_binder(output, sizeof(output), 4u, &size) ==
	       YUKARI_PARCEL_OK);
	assert(size == 24u && get32(output) == 0u &&
	       get32(output + 4u) == 0x73622a85u && get32(output + 8u) == 19u &&
	       get32(output + 20u) == 0u);
	memset(output, 0x8b, sizeof(output));
	memcpy(before, output, sizeof(output));
	size = 77u;
	assert(yukari_parcel_make_null_binder(output, 31u, 8u, &size) ==
	       YUKARI_PARCEL_NO_SPACE);
	assert(size == 77u && memcmp(output, before, sizeof(output)) == 0);
	assert(yukari_parcel_make_null_binder(output, sizeof(output), 16u, &size) ==
	       YUKARI_PARCEL_UNSUPPORTED);
	assert(size == 77u && memcmp(output, before, sizeof(output)) == 0);
	assert(yukari_parcel_filter_strings(output, 0u, output, sizeof(output),
					   bad_token, 1u, NULL) == YUKARI_PARCEL_INVALID);
}

static void test_corrupt_lengths_counts_and_overlap(void)
{
	unsigned char input[256] = {0}, output[256], before[256];
	const char *tokens[] = {"lineage"};
	struct yukari_parcel_result result = {0};
	size_t size, i;
	const unsigned int bad_count[] = {0xffffffffu, 8193u, 2u, 4u};

	memset(output, 0x23, sizeof(output));
	memcpy(before, output, sizeof(output));
	for (i = 0; i < sizeof(bad_count) / sizeof(bad_count[0]); ++i) {
		size = make_strings(input);
		put32(input + 4u, bad_count[i]);
		assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
						   tokens, 1u, &result) != YUKARI_PARCEL_OK);
		assert(memcmp(output, before, sizeof(output)) == 0);
	}
	size = make_strings(input);
	put32(input + 8u, 0xffffffffu);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	size = make_strings(input);
	input[32u] = 1; /* The first name's required terminating UTF-16 zero. */
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	size = make_strings(input);
	memcpy(before, input, sizeof(input));
	assert(yukari_parcel_filter_strings(input, size, input + 4u, sizeof(input) - 4u,
					   tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	assert(memcmp(input, before, sizeof(input)) == 0);
	size = make_debug(input);
	put32(input + 8u, 0u); /* Nonnullable parcelable cannot be null. */
	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
					      tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	size = make_debug(input);
	put32(input + 12u, 4u); /* No name or PID inside the parcelable. */
	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
					      tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
	size = make_debug(input);
	put32(input + 12u, 1000u); /* Bounded size that overruns the reply. */
	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
					      tokens, 1u, &result) == YUKARI_PARCEL_INVALID);
}

static void test_match_modes(void)
{
	unsigned char input[256] = {0}, output[256] = {0};
	const char *prefix_token[] = {"vendor."};
	const char *exact_token[] = {"vendor.lineage"};
	struct yukari_parcel_result result;
	size_t size = make_strings(input);

	assert(yukari_parcel_filter_strings_mode(input, size, output, sizeof(output),
						 prefix_token, 1u, YUKARI_PARCEL_MATCH_PREFIX,
						 &result) == YUKARI_PARCEL_OK);
	assert(result.kept == 2u && result.removed == 1u);
	assert(yukari_parcel_filter_strings_mode(input, size, output, sizeof(output),
						 exact_token, 1u, YUKARI_PARCEL_MATCH_EXACT,
						 &result) == YUKARI_PARCEL_OK);
	assert(result.kept == 2u && result.removed == 1u);
	assert(yukari_parcel_filter_strings_mode(input, size, output, sizeof(output),
						 exact_token, 1u, 99u, &result) ==
		       YUKARI_PARCEL_UNSUPPORTED);
}

static void test_replay_validation_and_matcher(void)
{
	unsigned char input[256] = {0}, output[256] = {0};
	const char *token[] = {"lineage"};
	struct yukari_parcel_result filtered = {0}, replayed = {0};
	int matched = 0;
	size_t end = 0, size;

	size = make_strings(input);
	assert(yukari_parcel_filter_strings(input, size, output, sizeof(output),
					   token, 1u, &filtered) == YUKARI_PARCEL_OK);
	assert(yukari_parcel_validate_strings(output, filtered.size, &replayed) ==
	       YUKARI_PARCEL_OK);
	assert(replayed.size == filtered.size && replayed.kept == filtered.kept &&
	       replayed.removed == 0u);
	assert(yukari_parcel_match_string16(input, size, 8u, token, 1u,
						 YUKARI_PARCEL_MATCH_SUBSTRING, &matched,
						 &end) == YUKARI_PARCEL_OK);
	assert(!matched && end > 8u);
	/* The second entry is the lineage name in this fixture. */
	assert(yukari_parcel_match_string16(input, size, end, token, 1u,
						 YUKARI_PARCEL_MATCH_SUBSTRING, &matched,
						 &end) == YUKARI_PARCEL_OK);
	assert(matched);
	put32(output + 4u, 99u);
	assert(yukari_parcel_validate_strings(output, filtered.size, &replayed) !=
	       YUKARI_PARCEL_OK);

	size = make_debug(input);
	assert(yukari_parcel_filter_debug_info(input, size, output, sizeof(output),
						 token, 1u, &filtered) == YUKARI_PARCEL_OK);
	assert(yukari_parcel_validate_debug_info(output, filtered.size, &replayed) ==
	       YUKARI_PARCEL_OK);
	assert(replayed.size == filtered.size && replayed.removed == 0u);
}

int main(void)
{
	test_strings();
	test_unicode_and_alignment();
	test_empty_all_hidden_and_tokens();
	test_every_truncation();
	test_debug();
	test_invalid_is_unchanged();
	test_limits_and_null_binder();
	test_corrupt_lengths_counts_and_overlap();
	test_match_modes();
	test_replay_validation_and_matcher();
	puts("parcel tests: ok");
	return 0;
}
