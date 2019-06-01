#include <stdio.h>
#include <stdarg.h>
#include "common.h"
#include <sawpads.h>
#include "lz4.h"

#define HEADER_SIZE sizeof(level_header_t)

static int card_initialized = 0;

static void init_card(save_progress_callback *pc)
{
	if (card_initialized == 0)
	{
		card_initialized = 1;
	}
}

void write_sjis_name(uint8_t *buffer, const char *format, ...)
{
	char text[32];

	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);

	int pos = 0;
	for (int i = 0; i < strlen(text); i++) {
		uint8_t c = text[i];
		if (c >= '0' && c <= '9') { buffer[pos++] = 0x82; buffer[pos++] = 0x4F + c - '0'; }
		else if (c >= 'A' && c <= 'Z') { buffer[pos++] = 0x82; buffer[pos++] = 0x60 + c - 'A'; }
		else if (c >= 'a' && c <= 'z') { buffer[pos++] = 0x82; buffer[pos++] = 0x81 + c - 'a'; }
		else /* space */ { buffer[pos++] = 0x81; buffer[pos++] = 0x40; }
	}
}

#define READ16(i) (secbuf[(i)] | (secbuf[(i)+1]<<8))
#define READ32(i) (secbuf[(i)] | (secbuf[(i)+1]<<8) | (secbuf[(i)+2]<<16) | (secbuf[(i)+3]<<24))
#define WRITE16(i, v) { secbuf[i] = ((v) & 0xFF); secbuf[i+1] = ((v) >> 8); }
#define WRITE32(i, v) { secbuf[i] = ((v) & 0xFF); secbuf[i+1] = (((v) >> 8) & 0xFF); secbuf[i+2] = (((v) >> 16) & 0xFF); secbuf[i+3] = ((v) >> 24); }

int load_level(int save_id, level_info *info, char *target, int32_t target_size, save_progress_callback *pc)
{
	uint8_t secbuf[128];
	int start_block = -1;
	int start_sector = 3;
	int16_t block_nexts[15];
	char filename[16];
	uint8_t *buffer;
	int sectors_read = 0;

	if (save_id < 0) return -1;
	snprintf(filename, sizeof(filename), "Fromage%d", save_id);

	// find start block and nexts
	for (int i = 1; i < 16; i++) {
		if (sawpads_read_card_sector(i, secbuf) <= 0) return -3;

		if ((secbuf[0] & 0x5C) == 0x50) {
			if (start_block < 0 && strcmp(secbuf + 10, filename) == 0) {
				start_block = i - 1;
			}
			block_nexts[i - 1] = (int16_t) (secbuf[8] | (secbuf[9] << 8));
		}
	}

	if (start_block < 0) return -2;

	// read header and parse it
	if (sawpads_read_card_sector((start_block + 1) * 64 + 2, secbuf) <= 0) return -3;
	int16_t sector_count = READ16(0x08);
	if (sector_count < 0) return -6;

	int cmp_data_size = ((sector_count - 1) * 128) + ((secbuf[0x0A] & 0x7F) + 1);

	info->xsize = READ16(0x02);
	info->ysize = READ16(0x04);
	info->zsize = READ16(0x06);
	info->cam_x = READ32(0x10);
	info->cam_y = READ32(0x14);
	info->cam_z = READ32(0x18);
	info->cam_rx = READ16(0x1C);
	info->cam_ry = READ16(0x1E);

	int target_data_size = info->xsize * info->ysize * info->zsize;
	if (target_data_size > target_size) return -4;

	info->hotbar_pos = secbuf[0x20] & 0x0F;
	for (int i = 0; i < HOTBAR_MAX; i++)
		info->hotbar_blocks[i] = secbuf[0x21 + i];

	buffer = malloc(sector_count * 128);
	if (buffer == NULL) return -4;

	// read data
	while (start_block >= 0) {
		while (start_sector < 64) {
			if (pc != NULL) pc(sectors_read, sector_count);
			if (sectors_read >= sector_count) break;

			if (sawpads_read_card_sector((start_block + 1) * 64 + start_sector, buffer + (sectors_read * 128)) <= 0) return -3;
			start_sector++; sectors_read++;
		}

		if (sectors_read >= sector_count) break;
		start_block = block_nexts[start_block];
		start_sector = 0;
	}

	// decompress data
	int data_size = LZ4_decompress_safe(buffer, target, cmp_data_size, target_data_size);
	free(buffer);
	if (data_size <= 0) return cmp_data_size;

	// that's all, folks!
	return 0;
}

int save_level(int save_id, level_info *info, const char *data, save_progress_callback *pc)
{
	uint8_t secbuf[128];
	uint8_t block_ids[15];
	int16_t block_nexts[15];
	uint8_t block_delete[15];
	char filename[16];

	if (save_id < 0) return -1;
	snprintf(filename, sizeof(filename), "Fromage%d", save_id);

	int level_size = info->xsize * info->ysize * info->zsize;
	int level_cmp_size = LZ4_compressBound(level_size);
	char *level_cmp_data = malloc(level_cmp_size);
	if (level_cmp_data == NULL) return -1;

	level_cmp_size = LZ4_compress_default(data, level_cmp_data, level_size, level_cmp_size);
	if (level_cmp_size <= 0) return -1;

	level_cmp_data = realloc(level_cmp_data, level_cmp_size);
	if (level_cmp_data == NULL) return -1;

	init_card(pc);

	int sectors_required = ((level_cmp_size + 127) / 128) + 3 /* header + title + icon frames */;
	int blocks_required = (sectors_required + 63) / 64;
	if (blocks_required > 15) {
		free(level_cmp_data);
		return -2;
	}

	int progress_max = sectors_required + 3 + 1;
	if (pc != NULL) pc(0, progress_max);

	int blocks_found = 0;

	// find free blocks
	// also write down the list of connected sectors, as well as
	// any sectors to be deleted
	for (int i = 1; i < 16; i++) {
		if (sawpads_read_card_sector(i, secbuf) <= 0) {
			free(level_cmp_data);
			return -3;
		}
		block_delete[i - 1] = 0;

		if ((secbuf[0] & 0xAC) == 0xA0) {
			block_ids[blocks_found++] = i;
			block_nexts[i - 1] = -1;
		} else if ((secbuf[0] & 0x5C) == 0x50) {
			if (strcmp(secbuf + 10, filename) == 0) {
				block_delete[i - 1] = 1;
			}
			block_nexts[i - 1] = (int16_t) (secbuf[8] | (secbuf[9] << 8));
		}
	}

	// delete blocks, if any
	for (int i = 0; i < 15; i++) {
		if (block_delete[i] == 1) {
			int j = i;
			while (j >= 0) {
				if (sawpads_read_card_sector(j + 1, secbuf) <= 0) {
					free(level_cmp_data);
					return -6;
				}
				secbuf[0] = (secbuf[0] & 0x0F) | 0xA0;
				if (sawpads_write_card_sector(j + 1, secbuf) <= 0) {
					free(level_cmp_data);
					return -6;
				}
				block_ids[blocks_found++] = j;
				j = block_nexts[j];
			}
		}
	}

	if (blocks_found < blocks_required) {
		free(level_cmp_data);
		return -4;
	}

	// allocate blocks
	memset(secbuf, 0, 128);
	for (int i = 0; i < blocks_required; i++) {
		secbuf[0] = (i == 0) ? 0x51 : ((i == (blocks_required - 1)) ? 0x53 : 0x52);
		if (i == (blocks_required - 1)) {
			secbuf[8] = secbuf[9] = 0xFF;
		} else {
			secbuf[8] = block_ids[i + 1] - 1;
			secbuf[9] = 0;
		}

		if (i == 0) {
			secbuf[5] = (blocks_required * 0x2000) >> 8;
			secbuf[6] = (blocks_required * 0x2000) >> 16;
			strcpy(secbuf + 0x0a, filename);
		} else {
			memset(secbuf + 0x04, 0, 0x04);
			memset(secbuf + 0x0a, 0, 0x20 - 0x0a);
		}

		secbuf[0x7F] = 0;
		for (int i = 0; i < 0x1F; i++) secbuf[0x7F] ^= secbuf[i];
		if (sawpads_write_card_sector(block_ids[i], secbuf) <= 0) {
			free(level_cmp_data);
			return i == 0 ? -3 : -5;
		}
	}
	if (pc != NULL) pc(1, progress_max);

	// write title frame
	memset(secbuf, 0, 128);
	secbuf[0] = 'S'; secbuf[1] = 'C';
	secbuf[2] = 0x11;
	secbuf[3] = 0x01;
	write_sjis_name(secbuf + 4, "Fromage (Slot %d)", save_id);
	if (sawpads_write_card_sector(block_ids[0] * 64, secbuf) <= 0) {
		free(level_cmp_data);
		return -5;
	}
	if (pc != NULL) pc(2, progress_max);

	// write icon frame
	memset(secbuf, 0, 128);
	if (sawpads_write_card_sector(block_ids[0] * 64 + 1, secbuf) <= 0) {
		free(level_cmp_data);
		return -5;
	}
	if (pc != NULL) pc(3, progress_max);

	// write fromage header frame
	memset(secbuf, 0, 128);
	secbuf[0x00] = 1;
	secbuf[0x01] = 0;
	WRITE16(0x02, info->xsize);
	WRITE16(0x04, info->ysize);
	WRITE16(0x06, info->zsize);
	WRITE16(0x08, sectors_required - 3);
	secbuf[0x0A] = ((level_cmp_size + 127) & 0x7F);
	WRITE32(0x10, info->cam_x);
	WRITE32(0x14, info->cam_y);
	WRITE32(0x18, info->cam_z);
	WRITE16(0x1C, info->cam_rx);
	WRITE16(0x1E, info->cam_ry);
	secbuf[0x20] = info->hotbar_pos & 0x0F;
	for (int i = 0; i < HOTBAR_MAX; i++)
		secbuf[0x21 + i] = info->hotbar_blocks[i];

	if (sawpads_write_card_sector(block_ids[0] * 64 + 2, secbuf) <= 0) {
		free(level_cmp_data);
		return -5;
	}

	// write fromage data frames
	for (int i = 3; i < sectors_required; i++) {
		if (pc != NULL) pc(i + 4, progress_max);
		uint8_t *ptr = level_cmp_data + (128 * (i - 3));
		if (sawpads_write_card_sector(block_ids[i >> 6] * 64 + (i & 0x3F), ptr) <= 0) {
			free(level_cmp_data);
			return -5;
		}
	}

	// done!
	free(level_cmp_data);
	return level_cmp_size;
}