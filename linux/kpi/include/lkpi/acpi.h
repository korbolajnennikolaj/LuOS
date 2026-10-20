// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_ACPI_H
#define _LKPI_ACPI_H

#include <lkpi/all.h>

typedef u64 acpi_physical_address;
typedef u16 acpi_owner_id;
#define ACPI_NAMESEG_SIZE 4
#define ACPI_NAME_SIZE 4
#define ACPI_OEM_ID_SIZE 6
#define ACPI_OEM_TABLE_ID_SIZE 8
#define ACPI_ID_LEN 16
#define ACPI_FLEX_ARRAY(TYPE, NAME) DECLARE_FLEX_ARRAY(TYPE, NAME)
#ifndef TRUE
#define TRUE (1 == 1)
#endif
#ifndef FALSE
#define FALSE (1 == 0)
#endif
#define return_ACPI_STATUS(s) return (s)

struct acpi_table_header {
	char signature[ACPI_NAMESEG_SIZE];
	u32 length;
	u8 revision;
	u8 checksum;
	char oem_id[ACPI_OEM_ID_SIZE];
	char oem_table_id[ACPI_OEM_TABLE_ID_SIZE];
	u32 oem_revision;
	char asl_compiler_id[ACPI_NAMESEG_SIZE];
	u32 asl_compiler_revision;
} __packed;

acpi_status acpi_get_table(const char *signature, u32 instance, struct acpi_table_header **out_table);
#define acpi_put_table(table) do { } while (0)
const char *acpi_format_exception(acpi_status status);

#define ACPI_VIDEO_DISPLAY_LCD 4
#define ACPI_VIDEO_NOTIFY_PROBE 0x81

#define acpi_has_method(handle, name) (false)
#define acpi_get_next_object(type, parent, child, next) (AE_NOT_FOUND)

#define acpi_dev_put(adev) do { } while (0)
#define acpi_dev_get(adev) (adev)
#define acpi_video_get_edid(adev, type, device_id, edid) (-ENODEV)

#endif
