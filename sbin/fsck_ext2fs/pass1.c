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
#include <stdlib.h>
#include <string.h>

#include "fsck_ext2fs.h"

static struct ext2fs_dinode dp;

/* Map an inode mode to the directory entry filetype byte. */
int
mode2ftype(mode_t mode)
{

	switch (mode & S_IFMT) {
	case S_IFREG:		return (EXT2_FT_REG_FILE);
	case S_IFDIR:		return (EXT2_FT_DIR);
	case S_IFCHR:		return (EXT2_FT_CHRDEV);
	case S_IFBLK:		return (EXT2_FT_BLKDEV);
	case S_IFIFO:		return (EXT2_FT_FIFO);
	case S_IFSOCK:		return (EXT2_FT_SOCK);
	case S_IFLNK:		return (EXT2_FT_SYMLINK);
	default:		return (EXT2_FT_UNKNOWN);
	}
}

/* pass1 block callback: mark the block used, detect duplicates. */
static int
pass1check(struct inodesc *idesc)
{
	uint64_t b = idesc->id_blkno;

	if (b >= (uint64_t)maxfsblock) {
		pwarn("BLOCK %ju OUT OF RANGE (ino %ju)\n", (uintmax_t)b,
		    (uintmax_t)idesc->id_ino);
		return (SKIP);
	}
	if (testbmap(b))
		pwarn("DUP BLOCK %ju (ino %ju)\n", (uintmax_t)b,
		    (uintmax_t)idesc->id_ino);
	setbmap(b);
	n_blks++;
	return (KEEPON);
}

void
pass1(void)
{
	uint32_t ipg, g;
	ino_t ino;

	ipg = le32toh(disk.d_fs.e2fs_ipg);
	for (g = 0; g < disk.d_gcount; g++) {
		inostathead[g].il_numalloced = ipg;
		inostathead[g].il_stat = calloc(ipg, sizeof(struct inostat));
		if (inostathead[g].il_stat == NULL)
			err(8, "cannot allocate inode stat array");
	}

	for (ino = 1; ino <= (ino_t)maxino; ino++) {
		struct inostat *stp;
		struct inodesc idesc;

		stp = &inostathead[(ino - 1) / ipg].il_stat[(ino - 1) % ipg];
		stp->ino_state = USTATE;
		if (ginode(ino, &dp) != 0)
			continue;
		if (le16toh(dp.e2di_mode) == 0)
			continue;		/* unallocated */

		stp->ino_state = (le16toh(dp.e2di_mode) & S_IFMT) == S_IFDIR ?
		    DSTATE : FSTATE;
		stp->ino_ftype = mode2ftype(le16toh(dp.e2di_mode));
		stp->ino_linkcnt = le16toh(dp.e2di_nlink);
		n_files++;
		memset(&idesc, 0, sizeof(idesc));
		idesc.id_ino = ino;
		idesc.id_func = pass1check;
		idesc.id_type = ADDR;
		ckinode(&dp, &idesc);
	}
}
