/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
#ifndef YUKARI_CONTROL_H_
#define YUKARI_CONTROL_H_

/* This header is shared by the Android userspace module and the LKM. */
#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
typedef __u32 yukari_u32;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t yukari_u32;
#endif

#define YUKARI_CONTROL_VERSION 1U
#define YUKARI_CONTROL_DEVICE "/dev/yukari_filter"
#define YUKARI_MAX_TARGET_UIDS 64U
#define YUKARI_MAX_FILTER_PATTERNS 8U
#define YUKARI_FILTER_PATTERN_SIZE 64U

enum yukari_match_mode {
	YUKARI_MATCH_PREFIX = 0,
	YUKARI_MATCH_SUBSTRING = 1,
	YUKARI_MATCH_EXACT = 2,
};

struct yukari_control_config {
	yukari_u32 version;
	yukari_u32 flags;
	yukari_u32 match_mode;
	yukari_u32 uid_count;
	yukari_u32 target_uids[YUKARI_MAX_TARGET_UIDS];
	yukari_u32 pattern_count;
	char patterns[YUKARI_MAX_FILTER_PATTERNS][YUKARI_FILTER_PATTERN_SIZE];
};

#define YUKARI_CONTROL_FLAG_KERNEL_ENABLED (1U << 0)
#define YUKARI_CONTROL_FLAG_USERSPACE_ENABLED (1U << 1)

#define YUKARI_IOCTL_SET_CONFIG _IOW('Y', 0x01, struct yukari_control_config)
#define YUKARI_IOCTL_CLEAR_CONFIG _IO('Y', 0x02)

#endif /* YUKARI_CONTROL_H_ */
