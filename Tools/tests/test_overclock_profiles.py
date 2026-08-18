#!/usr/bin/env python3
"""
Host-side tests for ExperimentalOverclockDxe safety policy.

These tests model the intended logic and cross-check the checked-in C sources.
They do not validate any real Tegra210 hardware behaviour.
"""

from __future__ import annotations

import os
import re


EFI_SUCCESS = "EFI_SUCCESS"
EFI_INVALID_PARAMETER = "EFI_INVALID_PARAMETER"
EFI_UNSUPPORTED = "EFI_UNSUPPORTED"
EFI_NOT_READY = "EFI_NOT_READY"
EFI_DEVICE_ERROR = "EFI_DEVICE_ERROR"
EFI_PROTOCOL_ERROR = "EFI_PROTOCOL_ERROR"

OC_CPU_FREQ_STOCK_KHZ = 1_020_000
OC_CPU_FREQ_MILD_KHZ = 1_530_000
OC_CPU_FREQ_MAX_CEILING_KHZ = 1_530_000

T210_SKU_ID_ERISTA = 0x83

OcProfileStock = 0
OcProfileMild = 1
OcProfileMax = OcProfileMild

PLLX_PARAMS = {
    OcProfileStock: {"cpu_freq_khz": OC_CPU_FREQ_STOCK_KHZ, "M": 1, "N": 53, "P": 1},
    OcProfileMild: {"cpu_freq_khz": OC_CPU_FREQ_MILD_KHZ, "M": 1, "N": 40, "P": 0},
}

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER_PATH = os.path.join(
    ROOT, "Drivers", "ExperimentalOverclockDxe", "ExperimentalOverclock.h"
)
SOURCE_PATH = os.path.join(
    ROOT, "Drivers", "ExperimentalOverclockDxe", "ExperimentalOverclockDxe.c"
)
DSC_PATH = os.path.join(ROOT, "NintendoSwitch.dsc")
DEC_PATH = os.path.join(ROOT, "NintendoSwitch.dec")


def pllx_output_khz(m: int, n: int, p: int) -> float:
    return 38_400 * n / (m * (2**p))


def consume_one_shot_variable(store: dict[str, int], name: str) -> int | None:
    value = store.pop(name, None)
    return value


def oc_apply_profile(
    profile: int,
    *,
    sku_id: int,
    thermal_validation_available: bool,
    power_validation_available: bool,
    pllx_will_lock: bool = True,
    rollback_will_succeed: bool = True,
) -> str:
    if profile < 0 or profile > OcProfileMax:
        return EFI_INVALID_PARAMETER

    params = PLLX_PARAMS[profile]
    if params["cpu_freq_khz"] > OC_CPU_FREQ_MAX_CEILING_KHZ:
        return EFI_INVALID_PARAMETER

    if (sku_id & 0xFF) != T210_SKU_ID_ERISTA:
        return EFI_UNSUPPORTED

    if profile == OcProfileStock:
        return EFI_SUCCESS

    if not thermal_validation_available:
        return EFI_NOT_READY

    if not power_validation_available:
        return EFI_UNSUPPORTED

    if not pllx_will_lock:
        return EFI_DEVICE_ERROR if rollback_will_succeed else EFI_PROTOCOL_ERROR

    return EFI_SUCCESS


class TestProfileTable:
    def test_only_stock_and_mild_profiles_exist(self):
        assert set(PLLX_PARAMS) == {OcProfileStock, OcProfileMild}
        assert OcProfileMax == OcProfileMild

    def test_all_profiles_respect_hard_ceiling(self):
        for params in PLLX_PARAMS.values():
            assert params["cpu_freq_khz"] <= OC_CPU_FREQ_MAX_CEILING_KHZ

    def test_stock_and_mild_pllx_outputs_match_expected_range(self):
        stock = PLLX_PARAMS[OcProfileStock]
        mild = PLLX_PARAMS[OcProfileMild]
        assert abs(pllx_output_khz(stock["M"], stock["N"], stock["P"]) - OC_CPU_FREQ_STOCK_KHZ) <= 50_000
        assert abs(pllx_output_khz(mild["M"], mild["N"], mild["P"]) - OC_CPU_FREQ_MILD_KHZ) <= 50_000


class TestFailClosedPolicy:
    def test_invalid_profile_rejected(self):
        assert oc_apply_profile(
            OcProfileMax + 1,
            sku_id=T210_SKU_ID_ERISTA,
            thermal_validation_available=False,
            power_validation_available=False,
        ) == EFI_INVALID_PARAMETER

    def test_non_erista_rejected(self):
        assert oc_apply_profile(
            OcProfileMild,
            sku_id=0x01,
            thermal_validation_available=True,
            power_validation_available=True,
        ) == EFI_UNSUPPORTED

    def test_missing_thermal_validation_fails_closed(self):
        assert oc_apply_profile(
            OcProfileMild,
            sku_id=T210_SKU_ID_ERISTA,
            thermal_validation_available=False,
            power_validation_available=True,
        ) == EFI_NOT_READY

    def test_missing_power_validation_fails_closed(self):
        assert oc_apply_profile(
            OcProfileMild,
            sku_id=T210_SKU_ID_ERISTA,
            thermal_validation_available=True,
            power_validation_available=False,
        ) == EFI_UNSUPPORTED

    def test_program_failure_reports_rollback_result(self):
        assert oc_apply_profile(
            OcProfileMild,
            sku_id=T210_SKU_ID_ERISTA,
            thermal_validation_available=True,
            power_validation_available=True,
            pllx_will_lock=False,
            rollback_will_succeed=True,
        ) == EFI_DEVICE_ERROR
        assert oc_apply_profile(
            OcProfileMild,
            sku_id=T210_SKU_ID_ERISTA,
            thermal_validation_available=True,
            power_validation_available=True,
            pllx_will_lock=False,
            rollback_will_succeed=False,
        ) == EFI_PROTOCOL_ERROR


class TestOneShotOptIn:
    def test_variable_is_consumed_before_use(self):
        store = {"NintendoSwitchOcProfile": OcProfileMild}
        assert consume_one_shot_variable(store, "NintendoSwitchOcProfile") == OcProfileMild
        assert "NintendoSwitchOcProfile" not in store

    def test_absent_variable_is_noop(self):
        store: dict[str, int] = {}
        assert consume_one_shot_variable(store, "NintendoSwitchOcProfile") is None


class TestSourceConsistency:
    @staticmethod
    def _extract_define(text: str, name: str) -> int:
        match = re.search(rf"#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)UL?", text)
        assert match, f"missing define {name}"
        value = match.group(1)
        return int(value, 16) if value.lower().startswith("0x") else int(value)

    def test_header_constants_match_expected_values(self):
        text = open(HEADER_PATH, encoding="utf-8").read()
        assert self._extract_define(text, "OC_CPU_FREQ_STOCK_KHZ") == OC_CPU_FREQ_STOCK_KHZ
        assert self._extract_define(text, "OC_CPU_FREQ_MILD_KHZ") == OC_CPU_FREQ_MILD_KHZ
        assert self._extract_define(text, "OC_CPU_FREQ_MAX_CEILING_KHZ") == OC_CPU_FREQ_MAX_CEILING_KHZ
        assert self._extract_define(text, "T210_SKU_ID_ERISTA") == T210_SKU_ID_ERISTA

    def test_source_uses_pllx_not_pllc(self):
        text = open(SOURCE_PATH, encoding="utf-8").read()
        assert "PLLX" in text
        assert "PLLC" not in text
        assert "EFI_NOT_READY" in text
        assert "EFI_PROTOCOL_ERROR" in text
        assert "OcConsumeRequestedProfile" in text

    def test_dec_default_opt_in_remains_false(self):
        text = open(DEC_PATH, encoding="utf-8").read()
        match = re.search(r"PcdExperimentalOverclockEnable\|(\w+)\|BOOLEAN", text)
        assert match
        assert match.group(1) == "FALSE"


class TestDscNoDuplicatePcds:
    def test_no_duplicates(self):
        import sys

        sys.path.insert(0, os.path.dirname(__file__))
        from check_duplicate_pcds import check_duplicates

        assert check_duplicates(DSC_PATH) == 0
