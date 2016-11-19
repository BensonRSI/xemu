/* Xemu - Somewhat lame emulation (running on Linux/Unix/Windows/OSX, utilizing
   SDL2) of some 8 bit machines, including the Commodore LCD and Commodore 65
   and some Mega-65 features as well.
   Copyright (C)2016 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "emutools.h"
#include "emutools_snapshot.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>


#ifdef XEMU_SNAPSHOT_SUPPORT


static int snapfd = -1;
static const char *framework_ident = "github.com/lgblgblgb/xemu";
static const Uint8 block_framing_id[] = { 'X','e','m','u','S','n','a','p' };
static const struct xemu_snapshot_definition_st *snapdef = NULL;
char xemusnap_error_buffer[XEMUSNAP_ERROR_BUFFER_SIZE * 2];
char xemusnap_user_error_buffer[XEMUSNAP_ERROR_BUFFER_SIZE];
static char *emu_ident;
static int last_sub_block_size_written = -1;


void xemusnap_close ( void )
{
	if (snapfd >= 0) {
		close(snapfd);
		snapfd = -1;
	}
}


void xemusnap_init ( const struct xemu_snapshot_definition_st *def, const char *ident )
{
	if (snapdef)
		return;
	snapdef = def;
	emu_ident = emu_malloc(strlen(ident) + strlen(framework_ident) + 8);
	sprintf(emu_ident, "Ident:%s:%s", framework_ident, ident);
	atexit(xemusnap_close);	
}


int xemusnap_read_file ( void *buffer, size_t size )
{
	size_t did = 0;
	while (did < size) {
		ssize_t r = read(snapfd, buffer, size);
		if (r < 0)
			return XSNAPERR_IO;
		if (!r)
			break;
		did += r;
		buffer += r;
	}
	if (!did)
		return XSNAPERR_NODATA;
	return did == size ? 0 : XSNAPERR_TRUNCATED;
}


int xemusnap_skip_file_bytes ( off_t size )
{
	return (lseek(snapfd, size, SEEK_CUR) == (off_t)-1) ? XSNAPERR_IO : 0;
}


int xemusnap_write_file ( const void *buffer, size_t size )
{
	size_t did = 0;
	while (did < size) {
		ssize_t r = write(snapfd, buffer, size);
		if (r < 0)
			return XSNAPERR_IO;
		if (!r)
			break;
		did += r;
		buffer += r;
	}
	return ((did == size) && did) ? 0 : XSNAPERR_NODATA;
}


int xemusnap_read_block_header ( struct xemu_snapshot_block_st *block )
{
	Uint8 buffer[256];
	// Read fixed size portion of the block header
	int ret = xemusnap_read_file(buffer, XEMUSNAP_FIXED_HEADER_SIZE);
	if (ret)
		return ret;
	// Check
	if (memcmp(buffer, block_framing_id, 8))
		return XSNAPERR_FORMAT;
	if (P_AS_BE32(buffer + 8) != XEMUSNAP_FRAMING_VERSION)
		return XSNAPERR_FORMAT;
	if (P_AS_BE32(buffer + 12))
		return XSNAPERR_FORMAT;
	block->block_version = P_AS_BE32(buffer + 16);
	block->header_size = buffer[20];
	block->idlen = block->header_size - XEMUSNAP_FIXED_HEADER_SIZE;
	if (block->idlen < 1 || block->idlen > XEMUSNAP_MAX_IDENT_LENGTH)
		return XSNAPERR_FORMAT;
	// Read the block ident string part
	ret = xemusnap_read_file(block->idstr, block->idlen);
	if (ret)
		return ret;
	block->idstr[block->idlen] = 0;
	return 0;
}


int xemusnap_write_block_header ( const char *ident, Uint32 version )
{
	int len = strlen(ident);
	Uint8 buffer[len + XEMUSNAP_FIXED_HEADER_SIZE];
	memcpy(buffer, block_framing_id, 8);
	U32_AS_BE(buffer + 8, XEMUSNAP_FRAMING_VERSION);
	U32_AS_BE(buffer + 12, 0);
	U32_AS_BE(buffer + 16, version);
	buffer[20] = len + XEMUSNAP_FIXED_HEADER_SIZE;
	memcpy(buffer + XEMUSNAP_FIXED_HEADER_SIZE, ident, len);
	last_sub_block_size_written = -1;
	return xemusnap_write_file(buffer, len + XEMUSNAP_FIXED_HEADER_SIZE);
}


int xemusnap_read_be32 ( Uint32 *result )
{
	Uint8 buffer[4];
	int ret = xemusnap_read_file(buffer, 4);
	*result = P_AS_BE32(buffer);
	return ret;
}


int xemusnap_skip_sub_blocks ( int num )
{
	do {
		Uint32 size;
		int ret = xemusnap_read_be32(&size);
		if (ret)
			return ret;
		if (!size)
			break;
		ret = xemusnap_skip_file_bytes(size);
		if (ret)
			return ret;
		num--;
	} while (num);
	return 0;
}


int xemusnap_write_sub_block ( const Uint8 *buffer, Uint32 size )
{
	int ret;
	Uint8 sizbuf[4];
	if (!size && !last_sub_block_size_written)
		FATAL("Xemu internal error while saving snapshot: there can be no two zero length sub-blocks together, as one is already signals end of sub-blocks!");
	last_sub_block_size_written = size;
	U32_AS_BE(sizbuf, size);
	ret = xemusnap_write_file(sizbuf, 4);
	if (ret)
		return ret;
	if (size) {
		ret = xemusnap_write_file(buffer, size);
		if (ret)
			return ret;
	}
	return 0;
}


#define RETURN_XSNAPERR(...)	\
	do {	\
		snprintf(xemusnap_error_buffer, sizeof xemusnap_error_buffer, __VA_ARGS__);	\
		return 1;	\
	} while (0)


static int load_from_open_file ( void )
{
	struct xemu_snapshot_block_st block;
	const struct xemu_snapshot_definition_st *def;
	block.counter = 0;
	block.sub_counter = -1;
	for (;;) {
		int ret = xemusnap_read_block_header(&block);
		if ((ret == XSNAPERR_NODATA) && block.counter)
			return 0;
		block.sub_counter = -1;
	handle_error:
		switch (ret) {
			case XSNAPERR_CALLBACK:
				RETURN_XSNAPERR("Error while loading snapshot block \"%s\" (%d/%d): %s", block.idstr, block.counter, block.sub_counter, xemusnap_user_error_buffer);
			case XSNAPERR_NODATA:
			case XSNAPERR_TRUNCATED:
				RETURN_XSNAPERR("Truncated file, unexpected end of file (~ %d/%d)", block.counter, block.sub_counter);
			case XSNAPERR_FORMAT:
				RETURN_XSNAPERR("Snapshot format error, maybe not a snapshot file, or version mismatch");
			case XSNAPERR_IO:
				RETURN_XSNAPERR("File I/O error while reading snapshot: %s", strerror(errno));
			case 0:
				block.is_ident = !memcmp(block.idstr, emu_ident, 6);
				def = snapdef;
				if (block.is_ident) {	// is it an ident block?
					if (block.counter)
						return 0;		// ident block other than the first one also signals end of snapshot, regardless of the rest of ident string
					// check the ident string if it's really our one
					if (strcmp(block.idstr + 6, emu_ident + 6))
						RETURN_XSNAPERR("Not our snapshot file, format is \"%s\", expected: \"%s\"", block.idstr + 6, emu_ident + 6);
				} else {
					if (!block.counter)
						RETURN_XSNAPERR("Invalid snapshot file, first block must be the ident block");
					for (;;) {
						if (!def->idstr)
							RETURN_XSNAPERR("Unknown block type: \"%s\"", block.idstr);
						if (!strcmp(def->idstr, block.idstr))
							break;
						def++;
					}
				}
				block.sub_counter = 0;
				for (;;) {
					ret = xemusnap_read_be32(&block.sub_size);
					if (ret)
						goto handle_error;
					if (!block.sub_size)
						break;
					if (block.is_ident) {
						ret = xemusnap_skip_sub_blocks(0);
						if (ret)
							goto handle_error;
					} else {
						strcpy(xemusnap_user_error_buffer, "?");
						ret = def->load(def, &block);
						if (ret)
							goto handle_error;
					}
					block.sub_counter++;
				}
				break;
			default:
				FATAL("Xemu snapshot load internal error: unknown xemusnap_read_block() answer: %d", ret);
		}
		block.counter++;
	}
}


int xemusnap_load ( const char *filename )
{
	xemusnap_close();
	snapfd = open(filename, O_RDONLY | O_BINARY);
	if (snapfd < 0)
		RETURN_XSNAPERR("Cannot open file: %s", strerror(errno));
	if (load_from_open_file()) {
		xemusnap_close();
		return 1;
	}
	xemusnap_close();
	return 0;
}


static int save_to_open_file ( void )
{
	const struct xemu_snapshot_definition_st *p = snapdef;
	int ret;
	// Ident block
	ret = xemusnap_write_block_header(emu_ident, 0);
	if (ret) goto handle_error;
	ret = xemusnap_write_sub_block(NULL, 0);
	if (ret) goto handle_error;
	// Walk on blocks, use saver callback to save them all
	while (p->idstr) {
		strcpy(xemusnap_user_error_buffer, "?");
		ret = p->save(p);
		if (ret) goto handle_error;
		ret = xemusnap_write_sub_block(NULL, 0);
		if (ret) goto handle_error;
		p++;
	}
	return 0;
handle_error:
	switch (ret) {
		case XSNAPERR_NODATA:
		case XSNAPERR_TRUNCATED:
			RETURN_XSNAPERR("Cannot write file");
		case XSNAPERR_FORMAT:
			RETURN_XSNAPERR("Format violation");
		case XSNAPERR_IO:
			RETURN_XSNAPERR("File I/O error while writing snapshot: %s", strerror(errno));
		case XSNAPERR_CALLBACK:
			RETURN_XSNAPERR("Error while saving snapshot block \"%s\": %s", p->idstr, xemusnap_user_error_buffer);
		default:
			FATAL("Xemu snapshot save internal error: unknown error code: %d", ret);
	}
}


int xemusnap_save ( const char *filename )
{
	xemusnap_close();
	snapfd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
	if (snapfd < 0)
		RETURN_XSNAPERR("Cannot create file: %s", strerror(errno));
	if (save_to_open_file()) {
		xemusnap_close();
		unlink(filename);
		return 1;
	}
	xemusnap_close();
	return 0;
}
#endif