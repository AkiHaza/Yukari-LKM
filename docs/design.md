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

Java and native Binder APIs share a lower-level transaction stream, but a
generic LKM cannot infer AIDL method semantics from that stream. A reliable
implementation must correlate the request and reply, select the exact
`IServiceManager.aidl` profile, parse each Parcel with bounds checks, and keep
Binder object offsets valid. The stock kernel module therefore fails open and
leaves framework-side reply filtering as the required integration point for
full list/get consistency.
