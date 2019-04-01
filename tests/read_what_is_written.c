#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <dhara/map.h>

#include "sim.h"
#include "util.h"

#define GC_RATIO 4

static const uint8_t raw_data1[] =
		{
				0x53, 0x42, 0x26, 0x01, 0x70, 0x00, 0x00, 0x20, 0x1A, 0x12, 0xAD, 0x2D,
				0x03, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xB0, 0xA3, 0x42, 0x03, 0x00, 0x00,
				0x00, 0x00, 0x1A, 0x8F, 0x7B, 0x3E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x22,
				0xC9, 0xFE, 0x56, 0x3E, 0x02, 0x00, 0x00, 0x00, 0x1A, 0x05, 0x61, 0xFD,
				0x01, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xE4, 0x42, 0xA8, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x22, 0x6C, 0x2A, 0xB0, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x22,
				0x58, 0x47, 0x9E, 0xB4, 0x02, 0x00, 0x00, 0x00, 0x1A, 0x37, 0x1D, 0xF7,
				0x01, 0x00, 0x00, 0x00, 0x00, 0x22, 0xDC, 0xD6, 0x21, 0xBE, 0x01, 0x00,
				0x00, 0x00, 0x22, 0xAC, 0x59, 0xE7, 0x19, 0x00, 0x00, 0x00, 0x00, 0x22,
				0x1C, 0x0C, 0x22, 0x6A, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xC3, 0x2D, 0xE8,
				0x02, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xFA, 0x2B, 0xA4, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x22, 0xD0, 0xDA, 0x1D, 0x9D, 0x00, 0x00, 0x00, 0x00, 0x1A,
				0x80, 0xA9, 0x09, 0x03, 0x00, 0x00, 0x00, 0x00, 0x22, 0x30, 0x6A, 0xFD,
				0x8A, 0x02, 0x00, 0x00, 0x00, 0x1A, 0x7C, 0x26, 0xD1, 0x02, 0x00, 0x00,
				0x00, 0x00, 0x22, 0x11, 0x65, 0xEB, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x22,
				0xA0, 0xF0, 0xCB, 0xA4, 0x02, 0x00, 0x00, 0x00, 0x1A, 0x38, 0xA9, 0xE0,
				0x02, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xF5, 0xF9, 0x71, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x22, 0x10, 0xE9, 0xAF, 0xDB, 0x01, 0x00, 0x00, 0x00, 0x1A,
				0x7C, 0xEA, 0x7A, 0x03, 0x00, 0x00, 0x00, 0x00, 0x22, 0x34, 0x40, 0xAF,
				0x45, 0x03, 0x00, 0x00, 0x00, 0x1A, 0xFB, 0xA6, 0x34, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x1A, 0x3A, 0xEE, 0xA9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x1A,
				0x96, 0xA4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xB6, 0x4A, 0x24,
				0x01, 0x00, 0x00, 0x00, 0x00, 0x1A, 0x75, 0xAA, 0x54, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x22, 0x58, 0x18, 0x9E, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x1A,
				0x75, 0x5D, 0x17, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0xB7, 0xC4
		};

static const uint8_t raw_data2[] =
		{
				0x12, 0x21, 0xFE, 0x00, 0x40, 0x00, 0x01, 0x00, 0xD2, 0x04, 0x00, 0x00,
				0x01, 0x00, 0x00, 0x00, 0x74, 0x01, 0x05, 0x00, 0x72, 0x01, 0x00, 0x00,
				0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0xE1, 0x07, 0x07, 0x1A, 0x03, 0x14, 0x25, 0x14, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0xE1, 0x07, 0x07, 0x1A, 0x03, 0x14, 0x25, 0x14, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x1E, 0xB4, 0xFE, 0xA6, 0xA9
		};

static const uint8_t raw_data3[] =
		{
				0x53, 0x42, 0x83, 0x00, 0x12, 0x24, 0x31, 0x0E, 0x01, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x0A, 0x00, 0x46, 0xFF
		};

static const uint8_t raw_data4[] =
		{
				0x53, 0x42, 0x83, 0x00, 0x12, 0x24, 0x31, 0x0E, 0x01, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x0A, 0x00, 0x46, 0xFF
		};

static uint8_t const *data_chunks[] = {raw_data1, raw_data2, raw_data3,
																			 raw_data4};
static size_t const data_sizes[] = {sizeof raw_data1, sizeof raw_data2,
																		sizeof raw_data3, sizeof raw_data4};

void single_sector_test(dhara_sector_t sector)
{
	printf("single sector test for sector#%d\n", sector);
	sim_reset();
	sim_inject_bad(30);
	sim_inject_timebombs(60, 10);
	sim_dump();

	const size_t page_size = 1 << sim_nand.log2_page_size;
	uint8_t page_buf[page_size];
	struct dhara_map map;

	uint8_t src_buf[page_size];
	uint8_t dst_buf[page_size];


	uint8_t const *pdata = data_chunks[0];
	size_t size = data_sizes[0];
	for (size_t i = 0; i < page_size; ++i) {
		src_buf[i] = 0xff;
		dst_buf[i] = 0xff;
	}
	memcpy(src_buf, pdata, size);

	dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
	dhara_map_resume(&map, NULL);


	dhara_error_t err = DHARA_E_NONE;
	if (dhara_map_write(&map, sector, src_buf, &err) < 0) {
		dabort("Error writing single sector", err);
	}
	dhara_map_sync(&map, NULL);

	for (int i = 0; i < 300; ++i) {
		printf("Writing sector #%d iteration #%d, data chunk #%d\n", sector, i, i & 0x03);
		dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
		dhara_map_resume(&map, NULL);

		dhara_page_t loc = 12900;

		if (dhara_map_find(&map, sector, &loc, &err) < 0) {
			dabort("Single sector not found", err);
		}

		uint8_t (*p)[512] = physical_page(loc);

		for (size_t j = 0; j < page_size; ++j) {
			dst_buf[j] = 0xff;
		}
		if (dhara_map_read(&map, sector, dst_buf, &err) < 0) {
			dabort("Error reading single sector", err);
		}
		if (memcmp(src_buf, dst_buf, page_size) != 0) {
			dabort("Single sector does not match", err);
		}

		unsigned data_idx = (i + 1) & 0x03;
		pdata = data_chunks[data_idx];
		size = data_sizes[data_idx];
		for (size_t i = 0; i < page_size; ++i) {
			src_buf[i] = 0xff;
			dst_buf[i] = 0xff;
		}
		memcpy(src_buf, pdata, size);
		if (dhara_map_write(&map, sector, src_buf, &err) < 0) {
			dabort("Error re-writing single sector", err);
		}
		dhara_map_sync(&map, NULL);
	}

	sim_dump();
}

int main()
{
	single_sector_test(0);
	single_sector_test(17);
	single_sector_test(34);
	single_sector_test(111);
	return 0;
}
