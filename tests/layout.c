/*
 * This file is part of the flashrom project.
 *
 * Copyright 2021 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <include/test.h>
#include <stdio.h>

#include "tests.h"
#include "../cros_ec_commands.h"
#include "flash.h"
#include "layout.h"
#include "libflashrom.h"
#include "programmer.h"

void included_regions_dont_overlap_test_success(void **state)
{
	(void) state; /* unused */

	printf("Creating layout... ");
	struct flashrom_layout *layout;
	assert_int_equal(0, flashrom_layout_new(&layout));
	printf("done\n");

	printf("Adding and including first region... ");
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00021000, 0x00031000, "first region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "first region"));
	printf("done");

	printf(", second (non-overlapping) region... ");
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00031001, 0x0023efc0, "second region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "second region"));
	printf("done\n");

	printf("Asserting included regions do not overlap... ");
	assert_int_equal(0, included_regions_overlap(layout));
	printf("done\n");

	printf("Releasing layout... ");
	flashrom_layout_release(layout);
	printf("done\n");
}

void included_regions_overlap_test_success(void **state)
{
	(void) state; /* unused */

	printf("Creating layout... ");
	struct flashrom_layout *layout;
	assert_int_equal(0, flashrom_layout_new(&layout));
	printf("done\n");

	printf("Adding and including first region... ");
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00021000, 0x00031000, "first region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "first region"));
	printf("done");

	printf(", second (overlapping) region... ");
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00027100, 0x0023efc0, "second region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "second region"));
	printf("done\n");

	printf("Asserting included regions overlap... ");
	assert_int_equal(1, included_regions_overlap(layout));
	printf("done\n");

	printf("Releasing layout... ");
	flashrom_layout_release(layout);
	printf("done\n");
}

void region_not_included_overlap_test_success(void **state)
{
	(void) state; /* unused */

	printf("Creating layout... ");
	struct flashrom_layout *layout;
	assert_int_equal(0, flashrom_layout_new(&layout));
	printf("done\n");

	printf("Adding and including first region... ");
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00021000, 0x00031000, "first region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "first region"));
	printf("done");

	printf(", second (overlapping) region, not included... ");
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00027100, 0x0023efc0, "second region"));
	printf("done\n");

	printf("Asserting included regions do not overlap... ");
	assert_int_equal(0, included_regions_overlap(layout));
	printf("done\n");

	printf("Releasing layout... ");
	flashrom_layout_release(layout);
	printf("done\n");
}

void layout_pass_sanity_checks_test_success(void **state)
{
	(void) state; /* unused */

	unsigned int region_start = 0x00021000;
	unsigned int region_end = 0x00031000;
	unsigned int region2_start = 0x00041000;
	unsigned int region2_end = 0x00051000;
	unsigned int start = 0;
	unsigned int len = 0;

	struct flashrom_layout *layout;

	printf("Creating layout with one included region... ");
	assert_int_equal(0, flashrom_layout_new(&layout));
	assert_int_equal(0, flashrom_layout_add_region(layout, region_start, region_end, "region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "region"));
	assert_int_equal(0, flashrom_layout_add_region(layout, region2_start, region2_end, "region2"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "region2"));
	assert_int_equal(0, flashrom_layout_exclude_region(layout, "region2"));
	printf("done\n");

	printf("Asserting region range... ");
	flashrom_layout_get_region_range(layout, "region", &start, &len);
	assert_int_equal(start, region_start);
	assert_int_equal(len, region_end - region_start + 1);
	printf("done\n");

	printf("Layout passes sanity checks... ");

	struct flashchip chip = {
		.total_size = 1024,
	};
	struct flashrom_flashctx flash = {
		.chip = &chip,
	};
	flashrom_layout_set(&flash, layout);
	assert_int_equal(0, layout_sanity_checks(&flash));

	printf("done\n");

	printf("Releasing layout... ");
	flashrom_layout_release(layout);
	printf("done\n");
}

void layout_region_invalid_address_test_success(void **state)
{
	(void) state; /* unused */

	struct flashrom_layout *layout;

	printf("Creating layout with one included region... ");
	assert_int_equal(0, flashrom_layout_new(&layout));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x60000000, 0x70000000, "region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "region"));
	printf("done\n");

	printf("Layout does not pass sanity checks... ");

	struct flashchip chip = {
		/* Make sure layout region addresses exceed total size on chip.  */
		.total_size = 1,
	};
	struct flashrom_flashctx flash = {
		.chip = &chip,
	};
	flashrom_layout_set(&flash, layout);
	assert_int_equal(1, layout_sanity_checks(&flash));

	printf("done\n");

	printf("Releasing layout... ");
	flashrom_layout_release(layout);
	printf("done\n");
}

void layout_region_invalid_range_test_success(void **state)
{
	(void) state; /* unused */

	struct flashrom_layout *layout;

	printf("Creating layout with one included region... ");
	assert_int_equal(0, flashrom_layout_new(&layout));
	/* Make sure address range of region is not positive i.e. start > end. */
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x00000020, 0x00000010, "region"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "region"));
	printf("done\n");

	printf("Layout does not pass sanity checks... ");

	struct flashchip chip = {
		/* Make sure layout region addresses fit into total size on chip.  */
		.total_size = 1024,
	};
	struct flashrom_flashctx flash = {
		.chip = &chip,
	};
	flashrom_layout_set(&flash, layout);
	assert_int_equal(1, layout_sanity_checks(&flash));

	printf("done\n");

	printf("Releasing layout... ");
	flashrom_layout_release(layout);
	printf("done\n");
}

void cros_ec_parse_layout_test_success(void **state)
{
	(void) state; /* unused */

	const struct ec_response_flash_region_info ro_region = {
		.offset = 0x000000,
		.size = 0x05f000,
	};
	const struct ec_response_flash_region_info rw_region = {
		.offset = 0x060000,
		.size = 0x060000,
	};
	struct flashrom_layout *layout;

	assert_int_equal(EC_IMAGE_RO | EC_IMAGE_RW,
			 cros_ec_parse_layout(NULL, &ro_region, &rw_region));

	assert_int_equal(0, flashrom_layout_new(&layout));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x000000, 0x05ffff, "WP_RO"));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x000000, 0x05dfff, "EC_RO"));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x000100, 0x00011f, "RO_FRID"));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x05e000, 0x05efff, "CBI"));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x05f000, 0x05ffff, "FW_PSTATE"));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x060000, 0x0bffff, "EC_RW"));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x0bffe0, 0x0bffff, "RW_FWID"));

	/* No included regions -> assume full image (RO | RW). */
	assert_int_equal(EC_IMAGE_RO | EC_IMAGE_RW,
			 cros_ec_parse_layout(layout, &ro_region, &rw_region));

	/* WP_RO and RO sub-regions inside EC_FLASH_REGION_RO -> EC_IMAGE_RO only. */
	assert_int_equal(0, flashrom_layout_include_region(layout, "WP_RO"));
	assert_int_equal(EC_IMAGE_RO, cros_ec_parse_layout(layout, &ro_region, &rw_region));
	assert_int_equal(0, flashrom_layout_exclude_region(layout, "WP_RO"));

	assert_int_equal(0, flashrom_layout_include_region(layout, "RO_FRID"));
	assert_int_equal(EC_IMAGE_RO, cros_ec_parse_layout(layout, &ro_region, &rw_region));
	assert_int_equal(0, flashrom_layout_exclude_region(layout, "RO_FRID"));

	assert_int_equal(0, flashrom_layout_include_region(layout, "CBI"));
	assert_int_equal(EC_IMAGE_RO, cros_ec_parse_layout(layout, &ro_region, &rw_region));
	assert_int_equal(0, flashrom_layout_exclude_region(layout, "CBI"));

	/* Dedicated PSTATE bank excluded from EC_FLASH_REGION_RO -> EC_IMAGE_UNKNOWN. */
	assert_int_equal(0, flashrom_layout_include_region(layout, "FW_PSTATE"));
	assert_int_equal(EC_IMAGE_UNKNOWN, cros_ec_parse_layout(layout, &ro_region, &rw_region));
	assert_int_equal(0, flashrom_layout_exclude_region(layout, "FW_PSTATE"));

	/* RW sub-region -> EC_IMAGE_RW only. */
	assert_int_equal(0, flashrom_layout_include_region(layout, "RW_FWID"));
	assert_int_equal(EC_IMAGE_RW, cros_ec_parse_layout(layout, &ro_region, &rw_region));

	/* Both RO_FRID and RW_FWID -> EC_IMAGE_RO | EC_IMAGE_RW. */
	assert_int_equal(0, flashrom_layout_include_region(layout, "RO_FRID"));
	assert_int_equal(EC_IMAGE_RO | EC_IMAGE_RW,
			 cros_ec_parse_layout(layout, &ro_region, &rw_region));

	flashrom_layout_release(layout);

	/* Default full-chip layout -> EC_IMAGE_RO | EC_IMAGE_RW. */
	assert_int_equal(0, flashrom_layout_new(&layout));
	assert_int_equal(0, flashrom_layout_add_region(layout, 0x000000, 0x0bffff, "complete flash"));
	assert_int_equal(0, flashrom_layout_include_region(layout, "complete flash"));
	assert_int_equal(EC_IMAGE_RO | EC_IMAGE_RW,
			 cros_ec_parse_layout(layout, &ro_region, &rw_region));
	flashrom_layout_release(layout);
}
