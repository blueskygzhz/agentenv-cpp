#!/bin/sh
# Stand-in VMM for the boot tests. Invoked exactly as the real binary is:
#   fake-firecracker --api-sock <path> --mmds-size-limit <n>
#
# Creates the API socket path so WaitForReady succeeds, then stays alive so the
# parent sees a running child rather than an early exit. The real VMM is then
# driven through the injected Connector, so no KVM is involved.
sock=""
while [ $# -gt 0 ]; do
  case "$1" in
    --api-sock) sock="$2"; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$sock" ] && : > "$sock"
exec sleep 30
