# Application routing: design in progress

This document describes proposed behavior. Application routing and its kill switch
are not implemented. Saving a rule does not protect or route any traffic.

Application routing is an optional feature; existing DNS profile handling is
independent. Each rule identifies an application and a network adapter using a
stable Windows adapter identifier. A disconnected or missing adapter must not
cause a protected application to fall back to another interface. Existing flows,
IPv4 and IPv6, TCP and UDP, startup, service failure, reboot, and rule removal
all need explicit verification. The application and its helper processes may
have different identities, and system DNS requests may not carry the requesting
application's identity.

Windows traditionally routes packets by IP destination. Recent Windows versions
also expose `FwpmConnectionPolicyAdd0` for application-specific routing. It is
absent from the `fwpuclnt.dll` on the development machine (Windows build 22631),
so availability must be checked at runtime. Microsoft's documentation also says
that route policies must be enabled separately. The effects of that global
setting and interaction with the guard are not verified.

WFP can identify applications and enforce blocks, but identifying a process
alone does not select an outbound interface. Another candidate is WFP bind redirection with a
kernel callout and interface-specific source addresses. That approach requires
validation for socket types, route selection, DNS, adapter changes, and Windows
driver signing; it is not yet selected or proven. A proxy or a suitable signed
existing component may be alternatives, subject to equivalent verification and
licensing review.

Protection must fail closed before any route is enabled and remain effective if
the UI exits or the enforcing component fails. Persistent and boot-time WFP
objects have different lifetimes. Installation and recovery must not leave stale
blocking policies without a documented, targeted removal path. A connected
adapter is not necessarily a working path to a destination.

Verification requires an isolated Windows test machine or VM with two usable
network interfaces and observable external endpoints. Test a protected and an
unprotected application simultaneously, disconnect and restore the selected
interface, force the enforcing component to exit, reboot, and verify IPv4,
IPv6, TCP, UDP, name resolution, and pre-existing connections. Do not assert a
kill switch is active until these tests demonstrate it.
