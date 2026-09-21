/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <stdlib.h>
#include <string.h>

#include "fsck_ext2fs.h"

const char *cdevname;
int	bflag;
int	ckclean;
int	debug;
int	fswritefd;
int	nflag;
int	preen;
int	skipclean = 1;
int	yflag;
int	maxfsblock;
int	maxino;
int	fsmodified;
int	uncorrected;
int64_t	n_blks;
int64_t	n_files;
char	*blockmap;
struct inostatlist *inostathead;
struct ext2fsd disk;
ino_t	*parentof;

void
fsckinit(void)
{
	cdevname = NULL;
	maxfsblock = maxino = 0;
	fsmodified = uncorrected = 0;
	n_blks = n_files = 0;
	free(blockmap);
	blockmap = NULL;
	inostathead = NULL;
	free(parentof);
	parentof = NULL;
	memset(&disk, 0, sizeof(disk));
}

/* Per-inode state for the inode number ino (1-based). */
struct inostat *
getinostat(ino_t ino)
{
	uint32_t ipg = le32toh(disk.d_fs.e2fs_ipg);

	return (&inostathead[(ino - 1) / ipg].il_stat[(ino - 1) % ipg]);
}
