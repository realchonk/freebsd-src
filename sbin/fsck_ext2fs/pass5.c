/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "fsck_ext2fs.h"

/*
 * Compare the block and inode bitmaps and the group descriptor counters
 * against what pass1 computed.  Report only.
 */
void
pass5(void)
{
	struct ext2fs *fs = &disk.d_fs;
	uint32_t bsize, bpg, ipg, first, g, i;
	uint64_t nfree, nifree, ndirs;
	char *bmap;

	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	bpg = le32toh(fs->e2fs_bpg);
	ipg = le32toh(fs->e2fs_ipg);
	first = le32toh(fs->e2fs_first_dblock);
	if ((bmap = malloc(bsize)) == NULL)
		err(8, "cannot allocate bitmap buffer");

	nfree = nifree = ndirs = 0;
	for (g = 0; g < disk.d_gcount; g++) {
		struct ext2_gd *gd = &disk.d_gd[g];
		uint64_t gfirst = first + (uint64_t)g * bpg;
		uint32_t gblocks = bpg, free_cnt = 0, ifree_cnt = 0;

		if (gfirst + gblocks > le32toh(fs->e2fs_bcount))
			gblocks = le32toh(fs->e2fs_bcount) - gfirst;
		if (ext2fs_bread(&disk, le32toh(gd->ext2bgd_b_bitmap), bmap,
		    bsize) != (ssize_t)bsize) {
			pwarn("GROUP %u: BLOCK BITMAP UNREADABLE\n", g);
			continue;
		}
		for (i = 0; i < gblocks; i++)
			if (isclr(bmap, i))
				free_cnt++;
		if (free_cnt != gd_nbfree(gd))
			pwarn("GROUP %u: FREE BLOCK COUNT WRONG IN "
			    "DESCRIPTOR (bitmap %u, descriptor %u)\n", g,
			    free_cnt, gd_nbfree(gd));
		nfree += gd_nbfree(gd);

		if (ext2fs_bread(&disk, le32toh(gd->ext2bgd_i_bitmap), bmap,
		    bsize) != (ssize_t)bsize) {
			pwarn("GROUP %u: INODE BITMAP UNREADABLE\n", g);
			continue;
		}
		for (i = 0; i < ipg; i++)
			if (isclr(bmap, i))
				ifree_cnt++;
		if (ifree_cnt != gd_nifree(gd))
			pwarn("GROUP %u: FREE INODE COUNT WRONG IN "
			    "DESCRIPTOR (bitmap %u, descriptor %u)\n", g,
			    ifree_cnt, gd_nifree(gd));
		nifree += gd_nifree(gd);
		ndirs += gd_ndirs(gd);
	}
	free(bmap);

	if (nfree != le32toh(fs->e2fs_fbcount))
		pwarn("FREE BLOCK COUNT WRONG IN SUPERBLOCK (groups %ju, "
		    "superblock %u)\n", (uintmax_t)nfree,
		    le32toh(fs->e2fs_fbcount));
	if (nifree != le32toh(fs->e2fs_ficount))
		pwarn("FREE INODE COUNT WRONG IN SUPERBLOCK (groups %ju, "
		    "superblock %u)\n", (uintmax_t)nifree,
		    le32toh(fs->e2fs_ficount));
	(void)ndirs;
}
