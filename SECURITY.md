# Security policy

This repository is **Biper**, a product fork of
[MeshCore](https://github.com/meshcore-dev/MeshCore). It is not MeshCore, and
the MeshCore maintainers are not responsible for anything in it. This file
replaces the upstream policy, which promised a process we do not run.

## What state this project is in

There is no tagged release in this repository. The current build is distributed through the web installer at [esp32ai.me/biper](https://esp32ai.me/biper/) with its SHA-256 published there — report issues against that hash. The approved-release manifest
ships with an empty artifact on purpose, and nothing here is recommended for
use where somebody's safety depends on it. Treat the code as work in the open,
not as a product you can deploy.

Radio behaviour, range, power draw and BLE against real phones are marked
`NOT MEASURED` in the README and they mean exactly that.

## Reporting something

**Please do not open a public issue for a security problem.** Use GitHub's
private vulnerability reporting on this repository: the **Security** tab →
**Report a vulnerability**.

Report it here if it concerns the Biper layer — everything under
`src/helpers/biper/`, `biper/`, `variants/biper_ap/`, or the four marked
`BIPER_AP hook` blocks in `examples/companion_radio/main.cpp`.

Report it [upstream](https://github.com/meshcore-dev/MeshCore/security) if it
concerns MeshCore itself: the mesh, routing, the companion protocol, or any
file this fork does not touch. We will forward anything that turns out to be
theirs, but going straight to them is faster for everyone.

## What you can expect

This is a one-person community project, so the honest answer is: no service
level. We read reports, we answer, and we say plainly when something will not
be fixed soon. There is no bounty. There are no supported older versions,
because there are no versions yet.

If a report describes a way to make somebody's device lie about what it is
doing, it goes to the front of the queue — that failure mode is the reason
this project exists.
