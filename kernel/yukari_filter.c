// SPDX-License-Identifier: GPL-2.0-only
/*
 * Yukari Binder policy probe.
 *
 * This module deliberately has a fail-open policy.  The generic Binder KMI
 * does not expose IServiceManager's Parcel schema, and a kretprobe on
 * binder_ioctl_write_read() runs after binder has copied the BR_* records to
 * the caller.  It therefore cannot safely rewrite a reply on all GKI
 * kernels.  The probe is useful for collecting caller diagnostics only.
 * Reply rewriting requires a separately patched Binder driver or framework
 * integration; this module does not provide that integration and never
 * changes the global service registry.
 */
#include <linux/cred.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uidgid.h>

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

static bool yukari_target_current(void)
{
	if (!enabled || target_uid < 10000)
		return false;
	return __kuid_val(current_euid()) == (uid_t)target_uid;
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
		pr_debug("yukari: binder ioctl observed for uid %d token=%s\n",
			 target_uid, hide_token ?: "");
	return 0;
}

static int __init yukari_init(void)
{
	memset(&binder_ioctl_probe, 0, sizeof(binder_ioctl_probe));
	binder_ioctl_probe.kp.symbol_name = "binder_ioctl_write_read";
	binder_ioctl_probe.handler = yukari_ioctl_ret;
	binder_ioctl_probe.entry_handler = NULL;
	binder_ioctl_probe.maxactive = 64;

	/* Probe registration is best effort.  GKI may hide this private symbol. */
	if (register_kretprobe(&binder_ioctl_probe)) {
		pr_info("yukari: binder_ioctl_write_read is unavailable; fail-open\n");
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
	pr_info("yukari: unloaded\n");
}

module_init(yukari_init);
module_exit(yukari_exit);
MODULE_DESCRIPTION("Yukari per-caller Binder service-view policy probe");
MODULE_AUTHOR("Yukari contributors");
MODULE_LICENSE("GPL");
