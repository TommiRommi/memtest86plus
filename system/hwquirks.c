// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2004-2023 Sam Demeulemeester
//
// ------------------------
// This file is used to detect quirks on specific hardware
// that require proprietary init here *OR* different code path
// later in various part of the code.
//
// Please add a quick comment for every quirk added to the list.

#include "hwquirks.h"
#include "io.h"
#include "pci.h"
#include "unistd.h"
#include "cpuinfo.h"
#include "cpuid.h"
#include "config.h"
#include "temperature.h"

quirk_t quirk;

// --------------------------------------
// -- Private quirk-specific functions --
// --------------------------------------

static void asus_tusl2_configure_mux(void)
{
    uint8_t muxreg;

    // Enter ASB100 Config Mode
    outb(0x87, 0x2E);
    outb(0x87, 0x2E);
    usleep(200);

    // Write LPC Command to access Config Mode Reg
    lpc_outb(0x7, 0x8);

    // Read Config Mode Register
    muxreg = lpc_inb(0xF1);

    // Change Smbus Mux Channel & Write Config Mode Register
    muxreg &= 0xE7;
    muxreg |= 0x10;
    lpc_outb(0xF1, muxreg);
    usleep(200);

    // Leave Config Mode
    outb(0xAA, 0x2E);
}

static void get_m1541_l2_cache_size(void)
{
    if (l2_cache != 0) {
        return;
    }

    // Check if L2 cache is enabled with L2CC-2 Register[0]
    if ((pci_config_read8(0, 0, 0, 0x42) & 1) == 0) {
        return;
    }

    // Get L2 Cache Size with L2CC-1 Register[3:2]
    uint8_t reg = (pci_config_read8(0, 0, 0, 0x41) >> 2) & 3;

    if (reg == 0b00) { l2_cache = 256; }
    if (reg == 0b01) { l2_cache = 512; }
    if (reg == 0b10) { l2_cache = 1024; }
}

static void disable_temp_reporting(void)
{
    enable_temperature = false;
}

static void amd_k8_revfg_temp(void)
{
    uint32_t rtcr = pci_config_read32(0, 24, 3, AMD_TEMP_REG_K8);

    // For Rev F & G, switch sensor if no temperature is reported
    if (!((rtcr >> 16) & 0xFF)) {
        pci_config_write8(0, 24, 3, AMD_TEMP_REG_K8, rtcr | 0x04);
    }

    // K8 Rev G Desktop requires an additional offset.
    if (cpuid_info.version.extendedModel < 6 && cpuid_info.version.extendedModel > 7)   // Not Rev G
        return;

    if (cpuid_info.version.extendedModel == 6 && cpuid_info.version.extendedModel < 9)  // Not Desktop
        return;

    uint16_t brandID = (cpuid_info.version.extendedBrandID >> 9) & 0x1f;

    if (cpuid_info.version.model == 0xF && (brandID == 0x7 || brandID == 0x9 || brandID == 0xC))   // Mobile (Single Core)
        return;

    if (cpuid_info.version.model == 0xB && brandID > 0xB)   // Mobile (Dual Core)
        return;

    cpu_temp_offset = 21.0f;
}

static void adl_unlock_smbus(void)
{
    uint16_t x = pci_config_read16(0, 31, 4, 0x04);

    if (!(x & 1)) {
        pci_config_write16(0, 31, 4, 0x04, x | 1);
    }
}

// ---------------------
// -- Public function --
// ---------------------

void quirks_init(void)
{
    quirk.id        = QUIRK_NONE;
    quirk.type      = QUIRK_TYPE_NONE;
    quirk.root_vid  = pci_config_read16(0, 0, 0, PCI_VID_REG);
    quirk.root_did  = pci_config_read16(0, 0, 0, PCI_DID_REG);
    quirk.process   = NULL;

    //  -------------------------
    //  -- ALi Aladdin V Quirk --
    //  -------------------------
    // As on many Socket 7 Motherboards, the L2 cache is external and must
    // be detected by a proprietary way based on chipset registers
    if (quirk.root_vid == PCI_VID_ALI && quirk.root_did == 0x1541) {    // ALi Aladdin V (M1541)
        quirk.id    = QUIRK_ALI_ALADDIN_V;
        quirk.type |= QUIRK_TYPE_MEM_SIZE;
        quirk.process = get_m1541_l2_cache_size;
    }

    //  ------------------------
    //  -- ASUS TUSL2-C Quirk --
    //  ------------------------
    // This motherboard has an ASB100 ASIC with a SMBUS Mux Integrated.
    // To access SPD later in the code, we need to configure the mux.
    // PS: Detection via DMI is unreliable, so using Root PCI Registers
    if (quirk.root_vid == PCI_VID_INTEL && quirk.root_did == 0x1130) {      // Intel i815
        if (pci_config_read16(0, 0, 0, PCI_SUB_VID_REG) == PCI_VID_ASUS) {  // ASUS
            if (pci_config_read16(0, 0, 0, PCI_SUB_DID_REG) == 0x8027) {    // TUSL2-C
                quirk.id    = QUIRK_TUSL2;
                quirk.type |= QUIRK_TYPE_SMBUS;
                quirk.process = asus_tusl2_configure_mux;
            }
        }
    }

    //  -------------------------------------------------
    //  -- SuperMicro X10SDV Quirk (GitHub Issue #233) --
    //  -------------------------------------------------
    // Memtest86+ crashs on Super Micro X10SDV motherboard with SMP Enabled
    // We were unable to find a solution so far, so disable SMP by default
    if (quirk.root_vid == PCI_VID_INTEL && quirk.root_did == 0x6F00) {             // Broadwell-E (Xeon-D)
        if (pci_config_read16(0, 0, 0, PCI_SUB_VID_REG) == PCI_VID_SUPERMICRO) {   // Super Micro
                quirk.id    = QUIRK_X10SDV_NOSMP;
                quirk.type |= QUIRK_TYPE_SMP;
                quirk.process = NULL;
        }
    }

    //  ------------------------------------------------------
    //  -- Early AMD K8 doesn't support temperature reading --
    //  ------------------------------------------------------
    // The on-die temperature diode on SH-B0/B3 stepping does not work.
    if (cpuid_info.vendor_id.str[0] == 'A' && cpuid_info.version.family == 0xF
        && cpuid_info.version.extendedFamily == 0 && cpuid_info.version.extendedModel == 0) {   // Early K8
        if ((cpuid_info.version.model == 4 && cpuid_info.version.stepping == 0) ||              // SH-B0 ClawHammer (Athlon 64)
            (cpuid_info.version.model == 5 && cpuid_info.version.stepping <= 1)) {              // SH-B0/B3 SledgeHammer (Opteron)
                quirk.id    = QUIRK_K8_BSTEP_NOTEMP;
                quirk.type |= QUIRK_TYPE_TEMP;
                quirk.process = disable_temp_reporting;
        }
    }

    //  ---------------------------------------------------
    //  -- Late AMD K8 (rev F/G) temp sensor workaround  --
    //  ---------------------------------------------------
    if (cpuid_info.vendor_id.str[0] == 'A' && cpuid_info.version.family == 0xF
        && cpuid_info.version.extendedFamily == 0 && cpuid_info.version.extendedModel >= 4) {   // Later K8

        quirk.id    = QUIRK_K8_REVFG_TEMP;
        quirk.type |= QUIRK_TYPE_TEMP;
        quirk.process = amd_k8_revfg_temp;
    }

    //  ------------------------------------------------
    //  -- AMD K10 CPUs Temp workaround (Errata #319) --
    //  ------------------------------------------------
    // Some AMD K10 CPUs on Socket AM2+/F have buggued thermal diode leading
    // to inaccurate temperature measurements. Affected steppings: DR-BA/B2/B3, RB-C2 & HY-D0.
    if (cpuid_info.vendor_id.str[0] == 'A' && cpuid_info.version.family == 0xF
        && cpuid_info.version.extendedFamily == 1 && cpuid_info.version.extendedModel == 0) {   // AMD K10

        uint8_t pkg_type = (cpuid_info.version.extendedBrandID >> 28) & 0x0F;
        uint32_t dct0_high = pci_config_read32(0, 24, 2, 0x94); // 0x94[8] = 1 for DDR3

        if (pkg_type == 0b0000 || (pkg_type == 0b0001 && (((dct0_high >> 8) & 1) == 0))) {      // Socket F or AM2+ (exclude AM3)

            if (cpuid_info.version.model < 4 ||                                                 // DR-BA, DR-B2 & DR-B3
                (cpuid_info.version.model == 4 && cpuid_info.version.stepping <= 2) ||          // RB-C2
                cpuid_info.version.model == 8) {                                                // HY-D0

                quirk.id    = QUIRK_AMD_ERRATA_319;
                quirk.type |= QUIRK_TYPE_TEMP;
                quirk.process = disable_temp_reporting;
            }
        }
    }

    //  --------------------------------------------------
    //  -- SMBus unlock for ADL-N (and probably others) --
    //  --------------------------------------------------
    if (imc_type == IMC_ADL_N && pci_config_read16(0, 31, 4, 0x2) == 0x54A3) {     // ADL-N
        quirk.id    = QUIRK_ADL_SMB_UNLOCK;
        quirk.type |= QUIRK_TYPE_SMBUS;
        quirk.process = adl_unlock_smbus;
    }
}
