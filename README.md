# EDK2 Implementation for Nintendo Switch (Tegra210)

## Status
Capable to boot something from SD card. If you have a Linux kernel with EFI stub support, supply the device tree for any Tegra210 device (not limited to Nintendo Switch) should boot.

ACPI also boots Windows and Linux, but limited devices are provided (only CPU at this moment).

## Device Support
- CPU services: GIC and Arch Timer.
- Clocks (reset, PLL, etc.)
- Power Management (PMC, PMIC & regulators, etc.)
- GPIO and Pin Multiplexor.
- MicroSD (should support SDSC, HC. XC probed and have partition table shown, but not intensively tested).
- Internal eMMC detection/read path exists, but is read-only and hardware-untested.
- Screen and FrameBuffer (need special [Coreboot](https://github.com/imbushuo/Coreboot))
- Side-band buttons, not yet registered as EFI Input Device.
- UART (Right Joy Con, 115200, 8n1)

## Windows Kernel Debugger Enablement

    bcdedit /store E:\EFI\Microsoft\Boot\BCD /set {default} debug on
    bcdedit /store E:\EFI\Microsoft\Boot\BCD /dbgsettings serial debugport:1 baudrate:115200
    
Plug in connector on the right-side Joy Con and connect to PC. Use WinDbg serial connection and it should work.

## Planned / In-Progress

- EHCI USB host
- eMMC
- Sideband buttons as input device
- Joy-Con (maybe not. Need high speed serial)

## GitHub Actions firmware validation

The `Host-side Validation` workflow now includes a full Ubuntu-hosted EDK2
AARCH64 build in addition to the existing Python/source checks.

- From a pull request, open the **Checks** tab (or the linked workflow run) for
  the commit you want to validate.
- From the **Actions** tab, select **Host-side Validation**, choose the branch
  or PR head commit you want, and use **Run workflow**.
- Download `full-edk2-aarch64-build-log` for the complete firmware build log.
- If the build succeeds, download
  `unvalidated-edk2-aarch64-firmware-build-output` for the generated firmware
  files.

Successful compilation is only a reproducible build check. It is not hardware
validation, does not enable eMMC writes, and does not make the firmware safe to
flash.

# Building Instructions

## Install dependencies
- sudo apt-get install build-essential uuid-dev acpica-tools git python3 python3-pip nasm gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
    ### Install Powershell
    - sudo apt-get update
    - sudo apt-get install -y wget apt-transport-https software-properties-common
    - wget -q https://packages.microsoft.com/config/ubuntu/20.04/packages-microsoft-prod.deb
    - sudo dpkg -i packages-microsoft-prod.deb
    - sudo apt-get update
    - sudo apt-get install -y powershell

## Clone Repositories
- git clone https://github.com/fail0verflow/shofel2.git
- git clone https://github.com/WolfLink115/Coreboot.git --recursive
- git clone --branch edk2-stable202605 https://github.com/tianocore/edk2.git --recursive
- git clone https://github.com/tianocore/edk2-platforms.git && git -C edk2-platforms checkout ae058185e12591a9a49e5895e90ca52936851973
- copy this repository into the edk2 workspace as `NintendoSwitchPkg`

`edk2-platforms` does not publish a matching `stable202605` branch/tag, so the
GitHub Actions workflow and the manual instructions above pin the exact
2026-05-22 commit that matches the validated workspace layout.

## Build ShofEL2 exploit, EDK2, and Coreboot
- cd shofel2/exploit && make
- cd ../edk2
- source edksetup.sh
- make -C BaseTools/
- export PACKAGES_PATH="$PWD:$PWD/../edk2-platforms"
- build -a AARCH64 -t GCC5 -p NintendoSwitchPkg/NintendoSwitch.dsc
- cd ../Coreboot && make nintendo_switch_defconfig && make

### Important safety note

Do not flash firmware or attempt to enable internal eMMC boot or experimental
overclocking paths until the full firmware build above succeeds and controlled
validation has been performed on expendable Erista hardware.

## After all that we should now have a coreboot.rom file in the build folder. Now we can try to boot edk2.
- cd ../shofel2/exploit
- sudo ./shofel2.py cbfs.bin ../../Coreboot/build/coreboot.rom

### If all went well, your Nintendo Switch should now be booting EDK2. Thank you for reading!

### If you run into a USB error, and you are using USB2.0, try using a USB3.0 port. If your pc doesn't have a USB3.0 port, there is a .patch file in the shofel2 source that you apply to your kernel. That will allow USB2.0 to work with shofel2. If you don't want to do that, or you just want to make things easier on yourself, you can use tegrarcmsmash.exe on a Windows install.

# Special thanks to

- Imbushuo for making the EDK2 Package and editing Coreboot sources to work with display.

- WolfLink115, mekb-turtle and tesla15 for their modifications on NintendoSwitchPkg

- Fail0verflow for making the shofel2 exploit and Switch Coreboot sources.

- The entirety of the Switch modding team for making this possible.
