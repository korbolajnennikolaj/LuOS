#include "power.h"

#include "acpi.h"
#include "acpi_types.h"
#include "checksum.h"
#include "components/logger.h"
#include "components/Memory/mm.h"
#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "fadt.h"
#include "tables.h"

#include <ports.h>
#include <stddef.h>

static bool acpi_parse_s5_in_bytes(const uint8_t *bytes, uint32_t len, uint8_t *out_a, uint8_t *out_b) {
    if (!bytes || len < 9) return false;

    for (uint32_t i = 0; i + 5 < len; i++) {
        if (!(bytes[i] == '_' && bytes[i + 1] == 'S' && bytes[i + 2] == '5' && bytes[i + 3] == '_')) {
            continue;
        }

        const uint8_t *p = bytes + i;

        bool prefixed = (i >= 1 && p[-1] == 0x08) ||
                         (i >= 2 && p[-2] == 0x08 && p[-1] == '\\');
        if (!prefixed) continue;

        if (p[4] != 0x12) continue;

        const uint8_t *q = p + 5;
        uint32_t extra_len_bytes = (q[0] & 0xC0) >> 6;
        q += extra_len_bytes + 2;

        if (bytes + len <= q + 1) continue;

        if (*q == 0x0A) q++;
        uint8_t typa = *q;
        q++;

        if (bytes + len <= q) continue;
        if (*q == 0x0A) q++;
        uint8_t typb = *q;

        *out_a = typa;
        *out_b = typb;
        return true;
    }

    return false;
}

bool acpi_find_s5_sleep_type(uint8_t *out_slp_typa, uint8_t *out_slp_typb) {
    if (!out_slp_typa || !out_slp_typb) return false;

    const ACPI_FADT *fadt = acpi_get_fadt();
    uint64_t dsdt_phys = fadt_get_dsdt_address(fadt);

    if (dsdt_phys) {
        const ACPI_SDT_HEADER *dsdt = (const ACPI_SDT_HEADER *)mm_phys_to_virt(dsdt_phys);

        if (dsdt && acpi_signature_matches(dsdt->Signature, "DSDT") &&
            dsdt->Length >= sizeof(ACPI_SDT_HEADER) &&
            acpi_checksum(dsdt, dsdt->Length)) {
            const uint8_t *body = (const uint8_t *)dsdt + sizeof(ACPI_SDT_HEADER);
            uint32_t body_len = dsdt->Length - sizeof(ACPI_SDT_HEADER);

            if (acpi_parse_s5_in_bytes(body, body_len, out_slp_typa, out_slp_typb)) {
                return true;
            }
        }
    }

    for (int i = 0; i < acpi_table_count(); i++) {
        ACPI_SDT_HEADER *hdr = acpi_get_table_by_index(i);
        if (!hdr || !acpi_signature_matches(hdr->Signature, "SSDT")) continue;

        const uint8_t *body = (const uint8_t *)hdr + sizeof(ACPI_SDT_HEADER);
        uint32_t body_len = hdr->Length > sizeof(ACPI_SDT_HEADER) ? hdr->Length - sizeof(ACPI_SDT_HEADER) : 0;

        if (acpi_parse_s5_in_bytes(body, body_len, out_slp_typa, out_slp_typb)) {
            return true;
        }
    }

    return false;
}

static void power_delay_us(uint64_t us) {
    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (tsc && tsc->get_tsc_ticks_per_ms && tsc->get_tsc_ticks_per_ms()) {
        tsc->sleep_tsc_us(us);
        return;
    }
    for (uint64_t i = 0; i < us; i++) inb(0x80);
}

static void acpi_enable_if_needed(const ACPI_FADT *fadt) {
    uint16_t cnt = 0;
    if (fadt_read_pm1_control(fadt, 0, &cnt) && (cnt & ACPI_PM1_CNT_SCI_EN)) return;

    uint16_t smi_port;
    uint8_t enable_value;
    if (!fadt_get_smi_command(fadt, &smi_port, &enable_value)) {
        LOG_DEBUG("SCI_EN is clear and FADT has no SMI command, writing PM1 control directly");
        return;
    }

    LOG_DEBUG("switching to ACPI mode via SMI port 0x%x", (unsigned)smi_port);
    outb(smi_port, enable_value);

    for (int i = 0; i < 300; i++) {
        if (fadt_read_pm1_control(fadt, 0, &cnt) && (cnt & ACPI_PM1_CNT_SCI_EN)) return;
        power_delay_us(1000);
    }

    LOG_WARNING("ACPI mode enable did not set SCI_EN, trying S5 anyway");
}

static void power_write_slp(const ACPI_FADT *fadt, int block_b, uint8_t slp_typ) {
    uint16_t cnt = 0;
    if (!fadt_read_pm1_control(fadt, block_b, &cnt)) {
        if (block_b) return;
        cnt = 0;
    }
    cnt &= (uint16_t)~((7u << ACPI_PM1_CNT_SLP_TYP_SHIFT) | ACPI_PM1_CNT_SLP_EN);
    cnt |= (uint16_t)((slp_typ & 7u) << ACPI_PM1_CNT_SLP_TYP_SHIFT);
    fadt_write_pm1_control(fadt, block_b, cnt);
    fadt_write_pm1_control(fadt, block_b, (uint16_t)(cnt | ACPI_PM1_CNT_SLP_EN));
}

bool acpi_shutdown(void) {
    if (!acpi_init()) {
        LOG_ERROR("shutdown failed: ACPI not available");
        return false;
    }

    const ACPI_FADT *fadt = acpi_get_fadt();
    if (!fadt || !fadt_has_fixed_hardware(fadt)) {
        LOG_ERROR("shutdown failed: no FADT / fixed hardware PM1 block");
        return false;
    }

    uint8_t slp_typa, slp_typb;
    if (!acpi_find_s5_sleep_type(&slp_typa, &slp_typb)) {
        LOG_ERROR("shutdown failed: could not find \\_S5 in DSDT/SSDT");
        return false;
    }

    acpi_enable_if_needed(fadt);

    LOG_INFO("entering S5 (SLP_TYPa=%u SLP_TYPb=%u, FADT rev %u, %u bytes)",
             (unsigned)slp_typa, (unsigned)slp_typb,
             (unsigned)fadt->Header.Revision, (unsigned)fadt->Header.Length);

    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    power_write_slp(fadt, 0, slp_typa);
    power_write_slp(fadt, 1, slp_typb);

    for (int i = 0; i < 3000; i++) power_delay_us(1000);

    if (flags & (1u << 9)) asm volatile("sti" ::: "memory");
    LOG_ERROR("S5 request was ignored by the firmware");
    return false;
}

static void kbc_wait_input_empty(void) {
    for (int i = 0; i < 0x10000; i++) {
        if ((inb(0x64) & 0x02) == 0) return;
        power_delay_us(2);
    }
}

static void reboot_via_8042(void) {
    for (int i = 0; i < 10; i++) {
        kbc_wait_input_empty();
        power_delay_us(50);
        outb(0x64, 0xFE);
        power_delay_us(50);
    }
}

static void reboot_via_cf9(void) {
    uint8_t cf9 = inb(0xCF9) & (uint8_t)~0x0E;
    outb(0xCF9, (uint8_t)(cf9 | 0x02));
    power_delay_us(50);
    outb(0xCF9, (uint8_t)(cf9 | 0x06));
    power_delay_us(50);
    outb(0xCF9, (uint8_t)(cf9 | 0x0E));
}

static void reboot_via_port92(void) {
    uint8_t v = inb(0x92);
    outb(0x92, (uint8_t)(v & ~0x01));
    power_delay_us(50);
    outb(0x92, (uint8_t)(v | 0x01));
}

static void reboot_via_triple_fault(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    asm volatile("lidt %0" :: "m"(null_idt));
    asm volatile("int3");
}

void acpi_reboot(void) {
    acpi_init();

    const ACPI_FADT *fadt = acpi_get_fadt();
    LOG_INFO("rebooting");

    asm volatile("cli" ::: "memory");

    if (fadt && fadt_reset_system(fadt)) {
        for (int i = 0; i < 500; i++) power_delay_us(1000);
        LOG_WARNING("FADT reset register ineffective");
    }

    reboot_via_8042();
    for (int i = 0; i < 500; i++) power_delay_us(1000);
    LOG_WARNING("8042 reset ineffective, trying 0xCF9");

    reboot_via_cf9();
    for (int i = 0; i < 500; i++) power_delay_us(1000);
    LOG_WARNING("0xCF9 reset ineffective, trying port 0x92");

    reboot_via_port92();
    for (int i = 0; i < 500; i++) power_delay_us(1000);
    LOG_WARNING("port 0x92 reset ineffective, forcing a triple fault");

    reboot_via_triple_fault();

    for (;;) {
        asm volatile("cli; hlt");
    }
}
