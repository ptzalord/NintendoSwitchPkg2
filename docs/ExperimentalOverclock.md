# Experimental Unsafe Overclocking for Erista / Tegra210 (HAC-001)

> **⚠ WARNING — READ BEFORE PROCEEDING ⚠**
>
> This feature is **EXPERIMENTAL** and **UNSAFE**.  It has **not** been
> validated on real hardware beyond basic bring-up.  Using it can cause:
>
> * System instability and random crashes
> * Data corruption on storage devices
> * Overheating and permanent hardware damage
> * Shortened hardware lifespan
> * Voided warranty
>
> **The authors and contributors accept no liability for damage caused by
> use of this feature.**

---

## Supported hardware

| Model | Silicon | Status |
|-------|---------|--------|
| Nintendo Switch HAC-001 (Erista) | Tegra210 (T210) | Driver compiled; **untested on real hardware** |
| Nintendo Switch HAC-001B (Mariko) | Tegra214 (T214) | **Refused at runtime — unsupported silicon** |
| Nintendo Switch Lite | Tegra214 | **Refused at runtime — unsupported silicon** |
| Nintendo Switch OLED | Tegra214 | **Refused at runtime — unsupported silicon** |

The driver reads `FUSE_SKU_INFO` at boot and refuses to activate on any
silicon that does not match the known Erista T210 identifier (0x83).

---

## Profiles and exact limits

| Profile ID | Name | CPU frequency | GPU frequency | Risk level |
|------------|------|---------------|---------------|------------|
| 0 | Stock | 1 020 MHz | 307 MHz (stock) | None – factory default |
| 1 | Mild | 1 530 MHz | unchanged | Moderate |
| 2 | Aggressive | 2 073.6 MHz | unchanged | High |

**Hard ceilings enforced in firmware (cannot be overridden):**

* CPU: 2 091 MHz absolute maximum.  Any profile or future extension that
  would exceed this value is rejected unconditionally by the driver.
* GPU: 998 MHz absolute maximum for future support, but direct GPCPLL
  programming from UEFI firmware is not currently
  implemented; GPU frequency remains at bootloader-configured stock value
  regardless of profile selection.

---

## How to activate (two explicit steps required)

Overclocking requires **both** of the following:

### Step 1 – Build-time opt-in (compile time)

Edit `NintendoSwitch.dsc` and set:

```
gNintendoSwitchPkgTokenSpaceGuid.PcdExperimentalOverclockEnable|TRUE
```

Rebuild the firmware and flash it.  With the default `FALSE` value the
`ExperimentalOverclockDxe` driver loads but exits immediately without
touching any register.

### Step 2 – Runtime opt-in (before each boot)

Set the UEFI variable from a UEFI shell or other UEFI environment:

```
# UEFI Shell – enable Mild overclocking (profile 1):
setvar NintendoSwitchOcProfile -guid 1900628e-0a8a-4099-8de5-f208ff80c4bf \
    -bs -rt =0x01

# UEFI Shell – revert to Stock (profile 0, same as deleting the variable):
setvar NintendoSwitchOcProfile -guid 1900628e-0a8a-4099-8de5-f208ff80c4bf \
    -bs -rt =0x00
```

Variable details:

| Property | Value |
|----------|-------|
| Name | `NintendoSwitchOcProfile` |
| GUID | `1900628e-0a8a-4099-8de5-f208ff80c4bf` |
| Type | `UINT8` (1 byte) |
| Value | `0` = Stock, `1` = Mild, `2` = Aggressive |

If the variable is absent the driver treats this as Stock (no-op).
Setting an unrecognised value (> 2) also results in no change.

---

## Activation flow (what the driver does)

1. Check `PcdExperimentalOverclockEnable`; exit immediately if `FALSE`.
2. Read the runtime UEFI variable; exit if absent or invalid.
3. Read `FUSE_SKU_INFO`; refuse if silicon is not Erista T210.
4. Validate the requested CPU frequency against the hard ceiling (2 091 MHz).
5. Attempt PLLC reprogramming with the pre-calculated M/N/P values.
6. On any failure: roll back to stock PLLC parameters and return.
7. Log a prominent warning confirming which profile was applied.

The driver **does not**:
* Silently raise CPU/GPU voltage.
* Program GPU clocks despite the documented future GPU ceiling.
* Program RAM/EMC clocks or set a default 2131 MHz RAM overclock.
* Disable thermal throttling or shutdown.
* Expose arbitrary MMIO/register-write interfaces.
* Auto-reapply the overclock after a failed or watchdog-reset boot.

---

## Thermal / voltage limitations (known gaps)

* **No thermal sensor driver**: UEFI does not currently include a validated
  Tegra210 thermal-sensor driver.  The driver cannot verify the die
  temperature before applying clocks.  A production-quality implementation
  would fail closed until temperature < 50 °C is confirmed.
* **No PMU (voltage) driver**: CPU core voltage is left at whatever level
  the bootloader configured.  For Mild and Aggressive profiles the
  bootloader-configured voltage may be insufficient, causing crashes rather
  than data corruption in most cases.  This is a known gap and a future
  improvement.
* **No boot-count guard**: the driver does not track failed boots.  If the
  system repeatedly crashes due to overclocking, delete the runtime variable
  before the next boot (see Recovery section).

---

## Recovery

If the system becomes unstable after enabling overclocking:

1. **Preferred**: Boot into a stable payload (e.g., a Linux image that boots
   without UEFI overclocking) and use UEFI Shell from there to delete or
   reset the `NintendoSwitchOcProfile` variable.
2. **Alternative**: Clear all UEFI variables by reflashing a stock firmware
   image.  This also deletes the overclock variable.
3. Once the variable is cleared (or set to `0`), the next UEFI boot will run
   at stock clocks even with the firmware image still present.

---

## Validation status

| Test | Method | Status |
|------|--------|--------|
| Profile bound checking | Python unit tests (`Tools/tests/test_overclock_profiles.py`) | ✅ Host-side only |
| Unsupported-hardware rejection | Python unit tests (logic simulation) | ✅ Host-side only |
| Rollback on error | Python unit tests (mock PLL failure) | ✅ Host-side only |
| PLLC register programming | Real Erista hardware | ❌ Not validated |
| Stability under load | Real Erista hardware | ❌ Not validated |
| Thermal behavior | Real Erista hardware | ❌ Not validated |

**Do not assume hardware validation has been performed.**  The driver
represents best-effort firmware-level clock programming only.

---

## References

* Tegra210 Technical Reference Manual (TRM) – Chapter 5 (Clock and Reset
  Controller), Chapter 9 (Fuses).
* Linux kernel `drivers/clk/tegra/clk-tegra210.c` for reference PLL
  programming sequences.
* Switchbrew / fail0verflow open-source Switch documentation.
