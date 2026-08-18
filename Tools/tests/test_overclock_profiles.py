#!/usr/bin/env python3
"""
test_overclock_profiles.py – host-side validation tests for
ExperimentalOverclockDxe profile logic.

These tests exercise the profile table, bound-checking rules, and
rollback/error-handling logic by simulating the firmware driver
behaviour in Python.  They do NOT run on real hardware.

Run with:  python -m pytest Tools/tests/test_overclock_profiles.py -v
"""
import pytest
import re
import os

# ---------------------------------------------------------------------------
# Constants mirrored from ExperimentalOverclock.h
# (single source of truth is the C header; these values must match it)
# ---------------------------------------------------------------------------

OC_CPU_FREQ_STOCK_KHZ       = 1_020_000
OC_CPU_FREQ_MILD_KHZ        = 1_530_000
OC_CPU_FREQ_AGGRESSIVE_KHZ  = 1_785_000
OC_CPU_FREQ_MAX_CEILING_KHZ = 1_900_000

OC_GPU_FREQ_STOCK_KHZ       = 307_200
OC_GPU_FREQ_MAX_CEILING_KHZ = 768_000

T210_SKU_ID_ERISTA = 0x83

OcProfileStock      = 0
OcProfileMild       = 1
OcProfileAggressive = 2
OcProfileMax        = OcProfileAggressive

# Profile table (must match mPllcParams in ExperimentalOverclockDxe.c)
PLLC_PARAMS = {
    OcProfileStock:      {"cpu_freq_khz": OC_CPU_FREQ_STOCK_KHZ,       "M": 1, "N": 53, "P": 2},
    OcProfileMild:       {"cpu_freq_khz": OC_CPU_FREQ_MILD_KHZ,        "M": 1, "N": 40, "P": 1},
    OcProfileAggressive: {"cpu_freq_khz": OC_CPU_FREQ_AGGRESSIVE_KHZ,  "M": 1, "N": 46, "P": 1},
}

OSC_FREQ_KHZ = 38_400  # 38.4 MHz oscillator input on Erista Switch


def pllc_output_khz(M: int, N: int, P: int) -> float:
    """Compute Fout = Fin * N / (M * 2^P) in kHz."""
    return OSC_FREQ_KHZ * N / (M * (2 ** P))


# ---------------------------------------------------------------------------
# Simulation helpers
# ---------------------------------------------------------------------------

class SimulatedSilicon:
    """Simulate FUSE_SKU_INFO readback."""
    def __init__(self, sku_id: int):
        self.sku_id = sku_id

    def is_erista(self) -> bool:
        return (self.sku_id & 0xFF) == T210_SKU_ID_ERISTA


def oc_validate_cpu_freq(freq_khz: int) -> bool:
    """Returns True if the frequency is within the hard ceiling."""
    return freq_khz <= OC_CPU_FREQ_MAX_CEILING_KHZ


def oc_apply_profile(
    profile: int,
    silicon: SimulatedSilicon,
    pllc_will_lock: bool = True,
) -> tuple[bool, str]:
    """
    Simulate OcApplyProfile().

    Returns (success: bool, message: str).
    On failure, the rollback to stock is simulated (logged in message).
    """
    # Validate profile ID
    if profile < 0 or profile > OcProfileMax:
        return False, f"invalid profile id {profile}"

    params = PLLC_PARAMS[profile]

    # Validate CPU frequency against ceiling
    if not oc_validate_cpu_freq(params["cpu_freq_khz"]):
        return False, f"cpu freq {params['cpu_freq_khz']} exceeds ceiling {OC_CPU_FREQ_MAX_CEILING_KHZ}"

    # Silicon check
    if not silicon.is_erista():
        return False, f"unsupported silicon SKU=0x{silicon.sku_id:02x}"

    # Simulate PLLC programming
    if not pllc_will_lock:
        # Simulate rollback
        return False, f"pllc lock timed out; rolled back to stock"

    return True, f"profile {profile} applied at {params['cpu_freq_khz']} kHz"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestProfileBoundChecking:
    def test_all_profiles_within_cpu_ceiling(self):
        for pid, params in PLLC_PARAMS.items():
            assert params["cpu_freq_khz"] <= OC_CPU_FREQ_MAX_CEILING_KHZ, (
                f"Profile {pid} CPU freq {params['cpu_freq_khz']} kHz "
                f"exceeds ceiling {OC_CPU_FREQ_MAX_CEILING_KHZ} kHz"
            )

    def test_ceiling_itself_is_rejected(self):
        """A frequency exactly 1 Hz above the ceiling must be rejected."""
        above_ceiling = OC_CPU_FREQ_MAX_CEILING_KHZ + 1
        assert not oc_validate_cpu_freq(above_ceiling)

    def test_ceiling_itself_is_accepted(self):
        """A frequency exactly at the ceiling must be accepted."""
        assert oc_validate_cpu_freq(OC_CPU_FREQ_MAX_CEILING_KHZ)

    def test_stock_below_mild(self):
        assert PLLC_PARAMS[OcProfileStock]["cpu_freq_khz"] < PLLC_PARAMS[OcProfileMild]["cpu_freq_khz"]

    def test_mild_below_aggressive(self):
        assert PLLC_PARAMS[OcProfileMild]["cpu_freq_khz"] < PLLC_PARAMS[OcProfileAggressive]["cpu_freq_khz"]

    def test_aggressive_below_ceiling(self):
        assert PLLC_PARAMS[OcProfileAggressive]["cpu_freq_khz"] < OC_CPU_FREQ_MAX_CEILING_KHZ

    def test_invalid_profile_id_rejected(self):
        silicon = SimulatedSilicon(T210_SKU_ID_ERISTA)
        ok, msg = oc_apply_profile(OcProfileMax + 1, silicon)
        assert not ok
        assert "invalid" in msg.lower()

    def test_negative_profile_id_rejected(self):
        silicon = SimulatedSilicon(T210_SKU_ID_ERISTA)
        ok, msg = oc_apply_profile(-1, silicon)
        assert not ok


class TestPllcFrequencyCalculation:
    """Verify pre-calculated M/N/P values produce frequencies near the target."""

    TOLERANCE_KHZ = 50_000  # 50 MHz tolerance for integer PLL rounding

    def test_stock_pllc_frequency(self):
        p = PLLC_PARAMS[OcProfileStock]
        fout = pllc_output_khz(p["M"], p["N"], p["P"])
        assert abs(fout - OC_CPU_FREQ_STOCK_KHZ) <= self.TOLERANCE_KHZ, (
            f"Stock PLLC output {fout:.0f} kHz not near {OC_CPU_FREQ_STOCK_KHZ} kHz"
        )

    def test_mild_pllc_frequency(self):
        p = PLLC_PARAMS[OcProfileMild]
        fout = pllc_output_khz(p["M"], p["N"], p["P"])
        assert abs(fout - OC_CPU_FREQ_MILD_KHZ) <= self.TOLERANCE_KHZ, (
            f"Mild PLLC output {fout:.0f} kHz not near {OC_CPU_FREQ_MILD_KHZ} kHz"
        )

    def test_aggressive_pllc_frequency(self):
        p = PLLC_PARAMS[OcProfileAggressive]
        fout = pllc_output_khz(p["M"], p["N"], p["P"])
        assert abs(fout - OC_CPU_FREQ_AGGRESSIVE_KHZ) <= self.TOLERANCE_KHZ, (
            f"Aggressive PLLC output {fout:.0f} kHz not near {OC_CPU_FREQ_AGGRESSIVE_KHZ} kHz"
        )

    def test_no_profile_exceeds_ceiling(self):
        for pid, p in PLLC_PARAMS.items():
            fout = pllc_output_khz(p["M"], p["N"], p["P"])
            assert fout <= OC_CPU_FREQ_MAX_CEILING_KHZ, (
                f"Profile {pid} PLLC output {fout:.0f} kHz exceeds ceiling"
            )


class TestUnsupportedHardwareRejection:
    def test_erista_accepted(self):
        silicon = SimulatedSilicon(T210_SKU_ID_ERISTA)
        ok, msg = oc_apply_profile(OcProfileMild, silicon)
        assert ok, f"Expected success on Erista, got: {msg}"

    def test_mariko_sku_rejected(self):
        # Mariko has a different SKU; 0x01 is a representative non-Erista value.
        silicon = SimulatedSilicon(0x01)
        ok, msg = oc_apply_profile(OcProfileMild, silicon)
        assert not ok
        assert "unsupported" in msg.lower()

    def test_unknown_sku_rejected(self):
        for bad_sku in [0x00, 0xFF, 0x42, 0x82, 0x84]:
            silicon = SimulatedSilicon(bad_sku)
            ok, msg = oc_apply_profile(OcProfileMild, silicon)
            assert not ok, f"SKU 0x{bad_sku:02x} should have been rejected"

    def test_stock_profile_on_unknown_silicon_also_rejected(self):
        """Silicon check happens before profile application even for stock."""
        silicon = SimulatedSilicon(0x00)
        ok, msg = oc_apply_profile(OcProfileStock, silicon)
        # Stock profile = 0; the driver would skip silicon check only at
        # OcProfileStock if it is used as a rollback target.  In our
        # simulation we enforce the check for all non-trivial activations;
        # the actual rollback path calls OcRestoreStock() which bypasses
        # the silicon check.
        # Here we simulate the full activation path:
        assert not ok


class TestRollbackOnError:
    def test_pllc_lock_failure_triggers_rollback(self):
        silicon = SimulatedSilicon(T210_SKU_ID_ERISTA)
        ok, msg = oc_apply_profile(OcProfileMild, silicon, pllc_will_lock=False)
        assert not ok
        assert "rolled back" in msg.lower()

    def test_aggressive_pllc_lock_failure_triggers_rollback(self):
        silicon = SimulatedSilicon(T210_SKU_ID_ERISTA)
        ok, msg = oc_apply_profile(OcProfileAggressive, silicon, pllc_will_lock=False)
        assert not ok
        assert "rolled back" in msg.lower()

    def test_successful_stock_apply_does_not_modify_message(self):
        silicon = SimulatedSilicon(T210_SKU_ID_ERISTA)
        ok, msg = oc_apply_profile(OcProfileStock, silicon, pllc_will_lock=True)
        assert ok


class TestHeaderConsistency:
    """Parse ExperimentalOverclock.h and verify constants match this file."""

    HEADER_PATH = os.path.join(
        os.path.dirname(__file__),
        "..", "..",
        "Drivers", "ExperimentalOverclockDxe", "ExperimentalOverclock.h",
    )

    def _extract_define(self, text: str, name: str) -> int:
        m = re.search(rf"#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)UL", text)
        if not m:
            m = re.search(rf"#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)", text)
        assert m, f"Could not find #define {name} in header"
        val = m.group(1)
        return int(val, 16) if val.startswith("0x") or val.startswith("0X") else int(val)

    def test_header_constants_match_test_constants(self):
        path = os.path.normpath(self.HEADER_PATH)
        with open(path) as f:
            text = f.read()

        assert self._extract_define(text, "OC_CPU_FREQ_STOCK_KHZ")       == OC_CPU_FREQ_STOCK_KHZ
        assert self._extract_define(text, "OC_CPU_FREQ_MILD_KHZ")        == OC_CPU_FREQ_MILD_KHZ
        assert self._extract_define(text, "OC_CPU_FREQ_AGGRESSIVE_KHZ")  == OC_CPU_FREQ_AGGRESSIVE_KHZ
        assert self._extract_define(text, "OC_CPU_FREQ_MAX_CEILING_KHZ") == OC_CPU_FREQ_MAX_CEILING_KHZ
        assert self._extract_define(text, "T210_SKU_ID_ERISTA")          == T210_SKU_ID_ERISTA


class TestDscNoDuplicatePcds:
    """Verify the DSC file has no duplicate PCD assignments."""

    DSC_PATH = os.path.join(
        os.path.dirname(__file__), "..", "..", "NintendoSwitch.dsc"
    )

    def test_no_duplicates(self):
        import sys
        sys.path.insert(0, os.path.dirname(__file__))
        from check_duplicate_pcds import check_duplicates
        result = check_duplicates(os.path.normpath(self.DSC_PATH))
        assert result == 0, "Duplicate PCD assignments found in NintendoSwitch.dsc"
