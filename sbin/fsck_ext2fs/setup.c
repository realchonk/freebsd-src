/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fstab.h>
#include <paths.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsck_ext2fs.h"

/* Backup superblock byte offset in group cg (newfs_ext2fs layout). */
static off_t
sblockloc(uint32_t cg)
{
	uint64_t first;
	uint32_t bsize;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	first = le32toh(disk.d_fs.e2fs_first_dblock) +
	    (uint64_t)cg * le32toh(disk.d_fs.e2fs_bpg);
	return ((off_t)first * bsize + (bsize == 1024 ? 0 : SBLOCKOFFSET));
}

/*
 * Try to read and verify a superblock at byte offset loc.
 */
static int
trysb(off_t loc)
{

	disk.d_sblockloc = loc;
	if (ext2fs_sbread(&disk) != 0)
		return (0);
	if (ext2fs_sbverify(&disk) != 0)
		return (0);
	return (1);
}

/*
 * Read the superblock: from -b if given, else the primary, else search
 * the backups.  Backup groups are cg 1 and odd cgs that are powers of
 * 3, 5 and 7; the primary's geometry is untrusted, so the search probes
 * candidate blocks for each legal block size.
 */
int
readsb(void)
{
	static const uint32_t bsizes[] = { 1024, 2048, 4096 };
	struct ext2fs *fs = &disk.d_fs;
	uint32_t bsize, cg, bpi;
	uint64_t blk;
	off_t loc;

	if (bflag != 0) {
		for (bpi = 0; bpi < nitems(bsizes); bpi++) {
			bsize = bsizes[bpi];
			loc = (off_t)bflag * bsize +
			    (bsize == 1024 ? 0 : SBLOCKOFFSET);
			if (trysb(loc))
				return (1);
		}
		return (0);
	}
	if (trysb(SBLOCKOFFSET))
		return (1);

	/* Probe backups at cg 1, 3, 5, 7, 9, 11 ... */
	for (cg = 1; cg < 64; cg += 2) {
		for (bpi = 0; bpi < nitems(bsizes); bpi++) {
			bsize = bsizes[bpi];
			blk = (bsize == 1024 ? 1 : 0) + (uint64_t)cg * bsize * 8;
			loc = (off_t)blk * bsize +
			    (bsize == 1024 ? 0 : SBLOCKOFFSET);
			if (trysb(loc))
				return (1);
		}
	}
	(void)fs;
	return (0);
}

int
openfilesys(const char *filesys)
{

	if (ext2fs_disk_fillout_blank(&disk, filesys) != 0)
		return (0);
	return (1);
}

/*
 * Open and validate.  Returns 0 on failure, 1 to run, and -1 when the
 * filesystem is clean and can be skipped.
 */
int
setup(const char *filesys)
{
	struct ext2fs *fs;
	uint32_t gcount;

	if (openfilesys(filesys) == 0 || readsb() == 0)
		return (0);
	if (ext2fs_gdread(&disk) == -1)
		return (0);
	fs = &disk.d_fs;
	gcount = disk.d_gcount;
	maxfsblock = (int)le32toh(fs->e2fs_bcount);
	maxino = (int)((uint64_t)gcount * le32toh(fs->e2fs_ipg));
	if ((blockmap = calloc(howmany(maxfsblock, 8), 1)) == NULL)
		err(8, "cannot allocate blockmap");
	if ((inostathead = calloc(gcount, sizeof(*inostathead))) == NULL)
		err(8, "cannot allocate inostathead");
	if ((parentof = calloc(maxino + 1, sizeof(*parentof))) == NULL)
		err(8, "cannot allocate parentof");
	if (skipclean && ckclean &&
	    le16toh(fs->e2fs_state) == E2FS_ISCLEAN)
		return (-1);
	return (1);
}
