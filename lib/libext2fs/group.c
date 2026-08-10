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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libext2fs.h>

/* Does block group cg carry a (backup) superblock? */
static int
ext2fs_cg_has_sb(const struct ext2fsd *disk, int cg)
{
	const struct ext2fs *fs;
	uint32_t compat, rocompat;
	int a3, a5, a7;

	fs = &disk->d_fs;

	if (cg == 0)
		return (1);

	compat = le32toh(fs->e2fs_features_compat);
	if (compat & EXT2F_COMPAT_SPARSESUPER2) {
		if (cg == (int)le32toh(fs->e4fs_backup_bgs[0]) ||
		    cg == (int)le32toh(fs->e4fs_backup_bgs[1]))
			return (1);
		return (0);
	}

	rocompat = le32toh(fs->e2fs_features_rocompat);
	if (cg <= 1 || (rocompat & EXT2F_ROCOMPAT_SPARSESUPER) == 0)
		return (1);
	if ((cg & 1) == 0)
		return (0);

	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= cg || a5 <= cg || a7 <= cg;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (cg == a3 || cg == a5 || cg == a7)
			return (1);
	return (0);
}

/* Filesystem block holding the number-th descriptor-table block. */
static uint64_t
ext2fs_cg_location(const struct ext2fsd *disk, int number, uint32_t bsize,
    uint32_t bpg, int has64, int has_meta_bg)
{
	const struct ext2fs *fs;
	int descpb, cg, logical_sb, has_super;

	fs = &disk->d_fs;

	logical_sb = bsize > SBLOCKSIZE ? 0 : 1;

	if (has_meta_bg == 0 ||
	    number < (int)le32toh(fs->e3fs_first_meta_bg))
		return ((uint64_t)(logical_sb + number + 1));

	descpb = has64 ? bsize / (int)sizeof(struct ext2_gd) :
	    bsize / (int)E2FS_REV0_GD_SIZE;
	cg = descpb * number;

	has_super = ext2fs_cg_has_sb(disk, cg) ? 1 : 0;

	return ((uint64_t)has_super + (uint64_t)cg * bpg +
	    le32toh(fs->e2fs_first_dblock));
}

/* Read and decode the group descriptor table into disk->d_gd. */
int
ext2fs_gdread(struct ext2fsd *disk)
{
	struct ext2fs *fs;
	uint32_t incompat, bsize, bpg, first_dblock, gdsize, descpb, gdbcount;
	uint64_t bcount;
	uint32_t gcount, nalloc, i, j, g_idx;
	int has64, has_meta_bg;
	char *buf;

	fs = &disk->d_fs;
	ERROR(disk, NULL);

	/* Free any prior table first. */
	free(disk->d_gd);
	disk->d_gd = NULL;
	disk->d_gcount = 0;

	incompat = le32toh(fs->e2fs_features_incompat);
	has64 = (incompat & EXT2F_INCOMPAT_64BIT) != 0;
	has_meta_bg = (incompat & EXT2F_INCOMPAT_META_BG) != 0;

	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	bpg = le32toh(fs->e2fs_bpg);
	first_dblock = le32toh(fs->e2fs_first_dblock);

	bcount = le32toh(fs->e2fs_bcount);
	if (has64)
		bcount |= (uint64_t)le32toh(fs->e4fs_bcount_hi) << 32;

	if (bpg == 0 || bcount <= first_dblock) {
		ERROR(disk, "invalid block group geometry");
		errno = EINVAL;
		return (-1);
	}

	/* 64 bytes with 64BIT, 32 bytes otherwise. */
	gdsize = has64 ? E2FS_64BIT_GD_SIZE : E2FS_REV0_GD_SIZE;
	descpb = bsize / gdsize;

	gcount = howmany(bcount - first_dblock, bpg);
	gdbcount = howmany(gcount, descpb);

	/* Allocate whole descriptor blocks; 64BIT copies them in one shot. */
	nalloc = (uint64_t)gdbcount * descpb;
	disk->d_gd = calloc(nalloc, sizeof(struct ext2_gd));
	buf = malloc(bsize);
	if (disk->d_gd == NULL || buf == NULL) {
		free(disk->d_gd);
		free(buf);
		disk->d_gd = NULL;
		ERROR(disk, "failed to allocate group descriptor table");
		errno = ENOMEM;
		return (-1);
	}

	g_idx = 0;
	for (i = 0; i < gdbcount; i++) {
		uint64_t blk;
		off_t off;

		blk = ext2fs_cg_location(disk, (int)i, bsize, bpg, has64,
		    has_meta_bg);
		off = (off_t)blk * bsize;
		if (pread(disk->d_fd, buf, bsize, off) != (ssize_t)bsize) {
			free(buf);
			free(disk->d_gd);
			disk->d_gd = NULL;
			ERROR(disk, "group descriptor table read failed");
			errno = EIO;
			return (-1);
		}

		if (has64) {
			/* Entries are already full struct ext2_gd on disk. */
			memcpy(disk->d_gd + (size_t)i * descpb, buf, bsize);
			g_idx = (size_t)(i + 1) * descpb;
			continue;
		}

		/* Rev0: 32-byte descriptors into 64-byte structs. */
		for (j = 0; j < descpb && g_idx < gcount; j++, g_idx++)
			memcpy(&disk->d_gd[g_idx], buf + (size_t)j * gdsize,
			    gdsize);
	}

	free(buf);
	disk->d_gcount = gcount;
	return (0);
}
