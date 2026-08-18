#!/usr/bin/env python3
"""
Host-side tests for MMC/eMMC Block I/O contract changes.

These tests model source-level logic and inspect the checked-in files. They do
not exercise real storage hardware.
"""

from __future__ import annotations

import os


ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
EFI_BLK_PATH = os.path.join(ROOT, "Drivers", "SdMmcDxe", "EfiBlkDeviceOp.c")
MMC_HOST_PATH = os.path.join(ROOT, "Drivers", "SdMmcDxe", "MmcHostOp.c")
SD_PATH = os.path.join(ROOT, "Drivers", "SdMmcDxe", "SdMmc.c")
EMMC_PATH = os.path.join(ROOT, "Drivers", "EmmcDxe", "EmmcDxe.c")
GITIGNORE_PATH = os.path.join(ROOT, ".gitignore")


def last_block_for_block_count(block_count: int) -> int:
    if block_count <= 0:
        raise ValueError("block_count must be positive")
    return block_count - 1


def read_request_is_valid(last_block: int, lba: int, buffer_size: int, block_size: int) -> bool:
    if buffer_size == 0:
        return True
    if block_size == 0 or buffer_size % block_size != 0:
        return False
    num_blocks = buffer_size // block_size
    if num_blocks == 0 or lba > last_block:
        return False
    return (num_blocks - 1) <= (last_block - lba)


def ext_csd_capacity_is_supported(high_capacity: bool, sectors: int) -> bool:
    return (not high_capacity) or sectors != 0


class TestLastBlockSemantics:
    def test_last_block_is_zero_based(self):
        assert last_block_for_block_count(1) == 0
        assert last_block_for_block_count(4096) == 4095

    def test_zero_capacity_is_rejected(self):
        try:
            last_block_for_block_count(0)
        except ValueError:
            pass
        else:
            raise AssertionError("zero-capacity media must be rejected")


class TestReadBounds:
    def test_zero_length_read_is_allowed(self):
        assert read_request_is_valid(last_block=99, lba=99, buffer_size=0, block_size=512)

    def test_end_boundary_is_allowed(self):
        assert read_request_is_valid(last_block=99, lba=99, buffer_size=512, block_size=512)

    def test_overrun_is_rejected(self):
        assert not read_request_is_valid(last_block=99, lba=99, buffer_size=1024, block_size=512)

    def test_bad_block_size_is_rejected(self):
        assert not read_request_is_valid(last_block=99, lba=0, buffer_size=513, block_size=512)


class TestExtCsdPolicy:
    def test_high_capacity_requires_nonzero_sector_count(self):
        assert ext_csd_capacity_is_supported(high_capacity=False, sectors=0)
        assert not ext_csd_capacity_is_supported(high_capacity=True, sectors=0)
        assert ext_csd_capacity_is_supported(high_capacity=True, sectors=1)


class TestSourceChecks:
    def test_emmc_is_configured_read_only(self):
        text = open(EMMC_PATH, encoding="utf-8").read()
        assert "BioConfigureInstance" in text
        assert "FALSE,\n             TRUE" in text or "FALSE,\r\n             TRUE" in text

    def test_sd_and_emmc_use_distinct_stable_device_paths(self):
        sd_text = open(SD_PATH, encoding="utf-8").read()
        emmc_text = open(EMMC_PATH, encoding="utf-8").read()
        assert "gSdMmcBlockIoDevicePathGuid" in sd_text
        assert "gEmmcBlockIoDevicePathGuid" in emmc_text

    def test_mmc_path_no_longer_asserts_for_expected_emmc_cases(self):
        text = open(MMC_HOST_PATH, encoding="utf-8").read()
        assert "ASSERT(FALSE)" not in text
        assert "mmc_send_ext_csd" in text
        assert "mForceMmcOnlyInit" in text
        assert "EXT_CSD_HS_TIMING" in text

    def test_block_io_source_contains_zero_based_lastblock_helper(self):
        text = open(EFI_BLK_PATH, encoding="utf-8").read()
        assert "BlockCount - 1" in text
        assert "Media->LastBlock - Lba" in text
        assert "EFI_WRITE_PROTECTED" in text

    def test_gitignore_covers_python_cache_artifacts(self):
        text = open(GITIGNORE_PATH, encoding="utf-8").read()
        assert "__pycache__/" in text
        assert "*.py[cod]" in text
