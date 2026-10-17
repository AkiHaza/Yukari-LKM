// SPDX-License-Identifier: GPL-2.0-only
/*
 * Yukari Binder policy probe.
 *
 * This module deliberately has a fail-open policy.  The generic Binder KMI
 * does not expose IServiceManager's Parcel schema, and a kretprobe on
 * binder_ioctl() runs after binder has copied the BR_* records to
 * the caller.  It therefore cannot safely rewrite a reply on all GKI
 * kernels.  The probe is useful for collecting caller diagnostics only.
 * Reply rewriting requires a separately patched Binder driver or framework
 * integration; this module does not provide that integration and never
 * changes the global service registry.
 */
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/kprobes.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include "control.h"

#define YUKARI_VERSION "0.1"

static int target_uid = -1;
module_param(target_uid, int, 0600);
MODULE_PARM_DESC(target_uid, "Diagnostic caller UID; values below 10000 disable observation");

static bool enabled;
module_param(enabled, bool, 0600);
MODULE_PARM_DESC(enabled, "Enable caller diagnostics in the registered Binder ioctl probe");

/* Retained as a diagnostic label; no Binder reply filtering occurs here. */
static char *hide_token = "lineage";
module_param(hide_token, charp, 0600);
MODULE_PARM_DESC(hide_token, "Diagnostic token label only; does not filter Binder replies");

static struct kretprobe binder_ioctl_probe;
static bool binder_ioctl_probe_registered;
static bool yukari_control_registered;
static DEFINE_MUTEX(yukari_config_lock);
static struct yukari_control_config yukari_config;

static bool yukari_application_uid(uid_t uid)
{
	/* Android reserves appIds below 10000 for system/core services. */
	return uid % 100000 >= 10000;
}

static long yukari_control_ioctl(struct file *file, unsigned int command,
				 unsigned long argument)
{
	struct yukari_control_config candidate;

	(void)file;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (_IOC_TYPE(command) != 'Y')
		return -ENOTTY;
	if (command == YUKARI_IOCTL_CLEAR_CONFIG) {
		mutex_lock(&yukari_config_lock);
		memset(&yukari_config, 0, sizeof(yukari_config));
		mutex_unlock(&yukari_config_lock);
		return 0;
	}
	if (command != YUKARI_IOCTL_SET_CONFIG ||
	    _IOC_SIZE(command) != sizeof(candidate) ||
	    copy_from_user(&candidate, (void __user *)argument, sizeof(candidate)))
		return -EINVAL;
	if (candidate.version != YUKARI_CONTROL_VERSION ||
	    candidate.flags & ~(YUKARI_CONTROL_FLAG_KERNEL_ENABLED |
					YUKARI_CONTROL_FLAG_USERSPACE_ENABLED) ||
	    candidate.uid_count > YUKARI_MAX_TARGET_UIDS ||
	    candidate.pattern_count > YUKARI_MAX_FILTER_PATTERNS ||
	    candidate.match_mode > YUKARI_MATCH_EXACT)
		return -EINVAL;
	{
		u32 i;
		for (i = 0; i < candidate.pattern_count; ++i) {
			char *pattern = candidate.patterns[i];
			char *end = memchr(pattern, '\0', YUKARI_FILTER_PATTERN_SIZE);
			if (!end || end == pattern)
				return -EINVAL;
			while (pattern < end) {
				if (*pattern < 0x21 || *pattern > 0x7e)
					return -EINVAL;
				++pattern;
			}
		}
	}
	mutex_lock(&yukari_config_lock);
	yukari_config = candidate;
	mutex_unlock(&yukari_config_lock);
	/* The stock GKI probe remains observational; this state is only a
	 * validated control-plane snapshot for a future vendor reply hook. */
	return 0;
}

static const struct file_operations yukari_control_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = yukari_control_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = yukari_control_ioctl,
#endif
};

static struct miscdevice yukari_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "yukari_filter",
	.fops = &yukari_control_fops,
	.mode = 0600,
};

static bool yukari_target_current(void)
{
	uid_t uid = __kuid_val(current_euid());
	u32 count;
	u32 i;

	if (!enabled)
		return false;
	if (READ_ONCE(yukari_config.version) == YUKARI_CONTROL_VERSION) {
		if (!(READ_ONCE(yukari_config.flags) & YUKARI_CONTROL_FLAG_KERNEL_ENABLED))
			return false;
		count = READ_ONCE(yukari_config.uid_count);
		if (count > YUKARI_MAX_TARGET_UIDS)
			return false;
		for (i = 0; i < count; ++i)
			if (READ_ONCE(yukari_config.target_uids[i]) == uid &&
			    yukari_application_uid(uid))
				return true;
		return false;
	}
	return target_uid >= 10000 && uid == (uid_t)target_uid &&
	       yukari_application_uid(uid);
}

/*
 * This handler is intentionally observational.  The return value is kept so
 * the kretprobe can be deployed on a stock GKI kernel without changing
 * Binder semantics.  It does not have access to a writable Binder reply
 * buffer and cannot implement reply filtering.
 */
static int yukari_ioctl_ret(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	(void)ri;
	(void)regs;
	if (yukari_target_current())
		pr_debug("yukari: binder ioctl observed for uid %u token=%s\n",
			 __kuid_val(current_euid()), hide_token ?: "");
	return 0;
}

static int __init yukari_init(void)
{
	int result;

	memset(&yukari_config, 0, sizeof(yukari_config));
	result = misc_register(&yukari_control_device);
	if (result) {
		pr_info("yukari: control device unavailable (%d); fail-open\n", result);
	} else {
		yukari_control_registered = true;
		pr_info("yukari: control device ready (state only; fail-open)\n");
	}
	memset(&binder_ioctl_probe, 0, sizeof(binder_ioctl_probe));
	binder_ioctl_probe.kp.symbol_name = "binder_ioctl";
	binder_ioctl_probe.handler = yukari_ioctl_ret;
	binder_ioctl_probe.entry_handler = NULL;
	binder_ioctl_probe.maxactive = 64;

	/* Probe registration is best effort.  GKI may hide this private symbol. */
	if (register_kretprobe(&binder_ioctl_probe)) {
		pr_info("yukari: binder_ioctl is unavailable; fail-open\n");
		return 0;
	}
	binder_ioctl_probe_registered = true;
	pr_info("yukari: loaded version %s (observational GKI mode)\n",
		YUKARI_VERSION);
	return 0;
}

static void __exit yukari_exit(void)
{
	if (binder_ioctl_probe_registered)
		unregister_kretprobe(&binder_ioctl_probe);
	if (yukari_control_registered)
		misc_deregister(&yukari_control_device);
	pr_info("yukari: unloaded\n");
}

module_init(yukari_init);
module_exit(yukari_exit);
MODULE_DESCRIPTION("Yukari per-caller Binder service-view policy probe");
MODULE_AUTHOR("Yukari contributors");
MODULE_LICENSE("GPL");
