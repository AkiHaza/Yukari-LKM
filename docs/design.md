# Design

The policy has three inputs: a trusted target identity, a list of literal
service-name tokens, and a caller operation. Matching is an AND between every
configured identity selector. With no selector configured, the filter is off.
This prevents an accidental empty configuration from affecting system
processes.

The service proxy runs the platform `service` executable with the same UID,
captures only its stdout, filters complete newline-delimited records, and
forwards the original exit status. stderr and non-list commands are left
alone. The process-local hook redirects exact `service list` and
`/system/bin/service list` calls made through the intercepted libc entry points
to this proxy. This covers the command forms that resolve through those PLT
calls; Android's Runtime.exec JNI fork/exec implementation, direct syscalls,
shell-wrapped commands, and direct Binder clients still require device
validation or a separate hook. The child process inherits the target UID, so
process-name matching for the proxy must be represented by the parent identity
rather than the child name `service`.

The userspace companion conditionally hooks the exported
`IPCThreadState::transact` PLT symbol on Android 14 (SDK 34). It selects the
Android 14 `IServiceManager` transaction codes for list, debug-info, name, and
declaration queries, validates the request header, and calls the shared
bounded Parcel codec. Hidden names receive the same not-found semantics across
`getService`, `checkService`, and `isDeclared`; unknown names and all malformed
or unsuccessful replies are left untouched. Missing symbols, internal
libbinder direct calls, other SDK versions, and non-empty vector object tables
fail open. The stock LKM remains observational and cannot cover direct
`BINDER_WRITE_READ`.

The Java `ServiceManager` cache is consulted before Binder for some well-known
names. A cached hit therefore remains outside this native transaction hook;
the consistency check must exercise an uncached target name.

After app specialization, the module calls `ServiceManager.listServices()`
once through JNI. This is the runtime self-check for the same transaction path:
the hook reports effective only after the real reply passes replay and
publication checks. Missing Java symbols, an exception, or a completed call
that never reaches the hook are logged with the unified `YukariBinder` tag and
leave the original result untouched.

The injected userspace library reads the root-owned
`/data/adb/modules/Yukari/config.json` independently in each target process.
The `/dev/yukari_filter` ioctl is only an optional, best-effort synchronization
channel for the observational kernel module. If it is absent or rejects a
snapshot, userspace filtering continues with its own validated configuration;
the kernel never parses JSON.
