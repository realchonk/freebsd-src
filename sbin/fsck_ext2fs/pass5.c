/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/types.h>

#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsck_ext2fs.h"

/*
 * Set the 16+16-bit little-endian counter lo/hi to v.
 */
static void
gd_set16(uint16_t *lo, uint16_t *hi, uint32_t v)
{

	*lo = htole16(v & 0xffff);
	*hi = htole16(v >> 16);
}

/*
 * The unused trailing bits of a bitmap block are set, never zero.
 */
static void
bmppad(char *map, uint32_t nbits, uint32_t bsize)
{
	uint32_t i, nbytes = howmany(nbits, NBBY);

	for (i = nbits; i < nbytes * NBBY; i++)
		setbit(map, i);
	memset(map + nbytes, 0xff, bsize - nbytes);
}

/*
 * Rebuild the block and inode bitmaps and the group descriptor and
 * superblock counters from what the earlier passes computed: blocks
 * claimed via blockmap plus each group's metadata layout, and inodes
 * allocated per inostat plus the reserved range.  Differences are
 * reported and, with approval, written back.
 */
void
pass5(void)
{
	struct ext2fs *fs = &disk.d_fs;
	struct inodesc idesc;
	char *bmap, *exp, *meta;
	uint64_t nfree, nifree;
	uint32_t bsize, bpg, ipg, first, isize, firstino;
	uint32_t bcount, gcount, gdsize, descpb, gdbcount, itb;
	uint32_t incompat, rocompat, desc_size, g;
	int gddirty, sbdirty;

	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	bpg = le32toh(fs->e2fs_bpg);
	ipg = le32toh(fs->e2fs_ipg);
	first = le32toh(fs->e2fs_first_dblock);
	bcount = le32toh(fs->e2fs_bcount);
	gcount = disk.d_gcount;
	isize = le32toh(fs->e2fs_rev) == E2FS_REV0 ? E2FS_REV0_INODE_SIZE :
	    le16toh(fs->e2fs_inode_size);
	firstino = le32toh(fs->e2fs_rev) == E2FS_REV0 ? EXT2_FIRSTINO :
	    le32toh(fs->e2fs_first_ino);
	itb = howmany(ipg * isize, bsize);
	incompat = le32toh(fs->e2fs_features_incompat);
	rocompat = le32toh(fs->e2fs_features_rocompat);
	gdsize = (incompat & EXT2F_INCOMPAT_64BIT) != 0 ?
	    E2FS_64BIT_GD_SIZE : E2FS_REV0_GD_SIZE;
	desc_size = le16toh(fs->e3fs_desc_size);
	if (desc_size < gdsize)
		desc_size = gdsize;
	descpb = bsize / gdsize;
	gdbcount = howmany(gcount, descpb);

	/* META_BG relocates the descriptor table itself, untracked here. */
	if (incompat & EXT2F_INCOMPAT_META_BG) {
		pwarn("META_BG NOT SUPPORTED; PASS 5 SKIPPED\n");
		return;
	}

	memset(&idesc, 0, sizeof(idesc));
	idesc.id_type = ADDR;
	idesc.id_fix = nflag ? IGNORE : DONTKNOW;

	if ((bmap = malloc(bsize)) == NULL || (exp = malloc(bsize)) == NULL)
		err(8, "cannot allocate bitmap buffers");
	if ((meta = calloc(howmany(maxfsblock, 8) + 1, 1)) == NULL)
		err(8, "cannot allocate metadata map");

	/*
	 * Metadata map over the whole filesystem: every group's bitmaps
	 * and inode table, and the superblock and descriptor-table blocks
	 * of groups carrying backups, wherever those blocks physically
	 * live (FLEX_BG moves them out of their nominal group).
	 */
	for (g = 0; g < gcount; g++) {
		struct ext2_gd *gd = &disk.d_gd[g];
		uint64_t gfirst = first + (uint64_t)g * bpg;
		uint32_t i;

		setbit(meta, le32toh(gd->ext2bgd_b_bitmap));
		setbit(meta, le32toh(gd->ext2bgd_i_bitmap));
		for (i = 0; i < itb; i++)
			setbit(meta, le32toh(gd->ext2bgd_i_tables) + i);
		if (ext2fs_cg_hassb(&disk, g))
			for (i = 0; i <= gdbcount; i++)
				setbit(meta, gfirst + i);
	}

	nfree = nifree = 0;
	gddirty = sbdirty = 0;
	for (g = 0; g < gcount; g++) {
		struct ext2_gd *gd = &disk.d_gd[g];
		struct inostat *istat = inostathead[g].il_stat;
		uint64_t gfirst = first + (uint64_t)g * bpg;
		uint32_t gblocks = bpg, free_cnt, ifree_cnt, ndirs;
		uint32_t i;
		int bmdiff;

		if (gfirst + gblocks > bcount)
			gblocks = bcount - gfirst;

		/*
		 * Expected block map: blocks claimed by inodes, plus the
		 * metadata that physically lives in this group.
		 */
		memset(exp, 0, bsize);
		for (i = 0; i < gblocks; i++)
			if (isset(meta, gfirst + i) || testbmap(gfirst + i))
				setbit(exp, i);
		bmppad(exp, gblocks, bsize);

		if (le16toh(gd->ext4bgd_flags) & EXT2_BG_BLOCK_UNINIT) {
			/* Bitmap never written: zeros are legitimate. */
			free_cnt = 0;
			for (i = 0; i < gblocks; i++)
				if (isclr(exp, i))
					free_cnt++;
			goto bcounters;
		}
		if (ext2fs_bread(&disk, le32toh(gd->ext2bgd_b_bitmap), bmap,
		    bsize) != (ssize_t)bsize) {
			pwarn("GROUP %u: BLOCK BITMAP UNREADABLE\n", g);
			goto inodebitmap;
		}
		bmdiff = 0;
		for (i = 0; i < gblocks; i++) {
			if (isset(exp, i) && isclr(bmap, i)) {
				pwarn("GROUP %u: BLOCK %ju MARKED FREE "
				    "BUT IN USE\n", g, (uintmax_t)(gfirst + i));
				bmdiff++;
			} else if (isclr(exp, i) && isset(bmap, i)) {
				pwarn("GROUP %u: BLOCK %ju MARKED IN USE "
				    "BUT FREE\n", g, (uintmax_t)(gfirst + i));
				bmdiff++;
			}
		}
		free_cnt = 0;
		for (i = 0; i < gblocks; i++)
			if (isclr(exp, i))
				free_cnt++;
		if (bmdiff != 0 && dofix(&idesc, "FIX BLOCK BITMAP") != 0) {
			memcpy(bmap, exp, bsize);
			if (ext2fs_bwrite(&disk, le32toh(gd->ext2bgd_b_bitmap),
			    bmap, bsize) == (ssize_t)bsize) {
				fsmodified = 1;
				if (rocompat & EXT2F_ROCOMPAT_METADATA_CKSUM) {
					ext2_gd_bbitmap_csum_update(
					    disk.d_csum_seed, bmap, bpg / 8,
					    gd, desc_size);
					gddirty = 1;
				}
			} else
				pwarn("GROUP %u: BLOCK BITMAP WRITE FAILED\n",
				    g);
		}

bcounters:
		if (free_cnt != gd_nbfree(gd)) {
			pwarn("GROUP %u: FREE BLOCK COUNT WRONG IN DESCRIPTOR "
			    "(computed %u, descriptor %u)\n", g, free_cnt,
			    gd_nbfree(gd));
			if (dofix(&idesc, "FIX") != 0) {
				gd_set16(&gd->ext2bgd_nbfree,
				    &gd->ext4bgd_nbfree_hi, free_cnt);
				gddirty = 1;
			}
		}
		nfree += free_cnt;

inodebitmap:
		/*
		 * Expected inode map: reserved inodes plus everything the
		 * earlier passes found allocated.
		 */
		memset(exp, 0, bsize);
		ifree_cnt = 0;
		ndirs = 0;
		for (i = 0; i < ipg; i++) {
			ino_t ino = (ino_t)g * ipg + i + 1;

			if ((uint64_t)ino < firstino ||
			    istat[i].ino_state != USTATE) {
				setbit(exp, i);
				if (istat[i].ino_ftype == EXT2_FT_DIR)
					ndirs++;
			} else
				ifree_cnt++;
		}
		bmppad(exp, ipg, bsize);

		if (le16toh(gd->ext4bgd_flags) & EXT2_BG_INODE_UNINIT) {
			/* Bitmap never written: zeros are legitimate. */
			nifree += ifree_cnt;
			if (ndirs != gd_ndirs(gd)) {
				pwarn("GROUP %u: DIRECTORY COUNT WRONG IN "
				    "DESCRIPTOR (computed %u, descriptor %u)\n",
				    g, ndirs, gd_ndirs(gd));
				if (dofix(&idesc, "FIX") != 0) {
					gd_set16(&gd->ext2bgd_ndirs,
					    &gd->ext4bgd_ndirs_hi, ndirs);
					gddirty = 1;
				}
			}
			continue;
		}
		if (ext2fs_bread(&disk, le32toh(gd->ext2bgd_i_bitmap), bmap,
		    bsize) != (ssize_t)bsize) {
			pwarn("GROUP %u: INODE BITMAP UNREADABLE\n", g);
			continue;
		}
		bmdiff = 0;
		for (i = 0; i < ipg; i++) {
			if (isset(exp, i) && isclr(bmap, i)) {
				pwarn("GROUP %u: INODE %ju MARKED FREE "
				    "BUT IN USE\n", g,
				    (uintmax_t)((uint64_t)g * ipg + i + 1));
				bmdiff++;
			} else if (isclr(exp, i) && isset(bmap, i)) {
				pwarn("GROUP %u: INODE %ju MARKED IN USE "
				    "BUT FREE\n", g,
				    (uintmax_t)((uint64_t)g * ipg + i + 1));
				bmdiff++;
			}
		}
		if (bmdiff != 0 && dofix(&idesc, "FIX INODE BITMAP") != 0) {
			memcpy(bmap, exp, bsize);
			if (ext2fs_bwrite(&disk, le32toh(gd->ext2bgd_i_bitmap),
			    bmap, bsize) == (ssize_t)bsize) {
				fsmodified = 1;
				if (rocompat & EXT2F_ROCOMPAT_METADATA_CKSUM) {
					ext2_gd_ibitmap_csum_update(
					    disk.d_csum_seed, bmap, ipg / 8,
					    gd, desc_size);
					gddirty = 1;
				}
			} else
				pwarn("GROUP %u: INODE BITMAP WRITE FAILED\n",
				    g);
		}

		if (ifree_cnt != gd_nifree(gd)) {
			pwarn("GROUP %u: FREE INODE COUNT WRONG IN DESCRIPTOR "
			    "(computed %u, descriptor %u)\n", g, ifree_cnt,
			    gd_nifree(gd));
			if (dofix(&idesc, "FIX") != 0) {
				gd_set16(&gd->ext2bgd_nifree,
				    &gd->ext4bgd_nifree_hi, ifree_cnt);
				gddirty = 1;
			}
		}
		nifree += ifree_cnt;

		if (ndirs != gd_ndirs(gd)) {
			pwarn("GROUP %u: DIRECTORY COUNT WRONG IN DESCRIPTOR "
			    "(computed %u, descriptor %u)\n", g, ndirs,
			    gd_ndirs(gd));
			if (dofix(&idesc, "FIX") != 0) {
				gd_set16(&gd->ext2bgd_ndirs,
				    &gd->ext4bgd_ndirs_hi, ndirs);
				gddirty = 1;
			}
		}
	}
	free(bmap);
	free(meta);
	free(exp);

	if (gddirty != 0) {
		if (ext2fs_gdwrite(&disk) == 0)
			fsmodified = 1;
		else
			pwarn("GROUP DESCRIPTOR TABLE WRITE FAILED\n");
	}

	if (nfree != le32toh(fs->e2fs_fbcount)) {
		pwarn("FREE BLOCK COUNT WRONG IN SUPERBLOCK (computed %ju, "
		    "superblock %u)\n", (uintmax_t)nfree,
		    le32toh(fs->e2fs_fbcount));
		if (dofix(&idesc, "FIX") != 0) {
			fs->e2fs_fbcount = htole32((uint32_t)nfree);
			sbdirty = 1;
		}
	}
	if (nifree != le32toh(fs->e2fs_ficount)) {
		pwarn("FREE INODE COUNT WRONG IN SUPERBLOCK (computed %ju, "
		    "superblock %u)\n", (uintmax_t)nifree,
		    le32toh(fs->e2fs_ficount));
		if (dofix(&idesc, "FIX") != 0) {
			fs->e2fs_ficount = htole32((uint32_t)nifree);
			sbdirty = 1;
		}
	}
	if (sbdirty != 0) {
		if (ext2fs_sbwrite(&disk, disk.d_sblockloc) == 0)
			fsmodified = 1;
		else
			pwarn("SUPERBLOCK WRITE FAILED\n");
	}
}
