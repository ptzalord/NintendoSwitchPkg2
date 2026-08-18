# Experimental overclock support

This repository keeps **stock clocks as the default**. The overclock driver is
still experimental and now **fails closed** unless runtime thermal and
power/voltage prerequisites can be validated.

## Supported profiles

| Profile | ID | Target CPU clock | Runtime behaviour |
|---|---:|---:|---|
| Stock | 0 | 1020 MHz | No change |
| Mild experimental | 1 | 1530 MHz | Refused unless validated prerequisites exist |

There is **no aggressive 2+ GHz profile** in this tree.

## Safety policy

- Build-time opt-in is still required through
  `gNintendoSwitchPkgTokenSpaceGuid.PcdExperimentalOverclockEnable|TRUE`.
- Runtime opt-in uses the `NintendoSwitchOcProfile` variable.
- The runtime variable is treated as **one-shot**: it is consumed and deleted
  before any non-stock profile would be applied.
- Unsupported silicon is rejected.
- Frequencies above **1.53 GHz** are rejected unconditionally.
- If profile programming fails after being attempted, rollback to stock is
  attempted and rollback failure is reported distinctly.
- If validated thermal or power/voltage checks are unavailable, the driver
  returns `EFI_NOT_READY` / `EFI_UNSUPPORTED` semantics and leaves stock clocks
  in effect.

## Current limitation in this repository

This repository does **not** currently contain validated runtime support for:

- Tegra210 thermal prerequisite checks, or
- runtime CPU power/voltage validation.

Because those checks are unavailable, the implementation intentionally
**does not apply non-stock clocks on hardware today**.

## Scope of host-side tests

The tests under `Tools/tests/` check source constants, one-shot opt-in logic,
fail-closed policy, and rollback status handling. They do **not** prove:

- correct hardware bring-up,
- stable PLL switching on Erista,
- safe thermals,
- safe voltage margins, or
- long-run system stability.

## Before anyone enables this

No user should flash firmware or attempt to enable these paths until:

1. a full firmware build succeeds from source, and
2. controlled validation is performed on expendable **Erista** hardware.
