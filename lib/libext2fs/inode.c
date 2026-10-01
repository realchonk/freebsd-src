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

#include <libext2fs.h>

/*
 * Locate inode ino: the filesystem block and byte offset within it.
 * Raw devices only accept sector-aligned I/O, so inode access goes
 * through whole filesystem blocks.
 */
static int
ext2fs_inoloc(const struct ext2fsd *disk, ino_t ino, uint64_t *blkp,
    uint32_t *boffp, uint32_t *isizep)
{
	const struct ext2fs *fs;
	const struct ext2_gd *gd;
	uint32_t ipg, idx;

	fs = &disk->d_fs;
	if (ino == 0 || disk->d_gd == NULL ||
	    ino > (ino_t)disk->d_gcount * le32toh(fs->e2fs_ipg)) {
		errno = EINVAL;
		return (-1);
	}

	*isizep = le32toh(fs->e2fs_rev) == E2FS_REV0 ? E2FS_REV0_INODE_SIZE :
	    le16toh(fs->e2fs_inode_size);
	/*
	 * Sizes beyond the understood struct (e.g. the 256 bytes of
	 * e2fsprogs-created filesystems) are fine: the extra bytes hold
	 * extended fields that callers do not interpret.
	 */
	if (*isizep < E2FS_REV0_INODE_SIZE ||
	    *isizep > (1024u << le32toh(fs->e2fs_log_bsize))) {
		errno = EINVAL;
		return (-1);
	}

	ipg = le32toh(fs->e2fs_ipg);
	idx = (uint32_t)(ino - 1);

	gd = &disk->d_gd[idx / ipg];
	idx %= ipg;
	*blkp = ((uint64_t)le32toh(gd->ext2bgd_i_tables) |
	    (uint64_t)le32toh(gd->ext4bgd_i_tables_hi) << 32) +
	    (uint64_t)idx * *isizep / (1024u << le32toh(fs->e2fs_log_bsize));
	*boffp = (uint64_t)idx * *isizep %
	    (1024u << le32toh(fs->e2fs_log_bsize));
	return (0);
}

/* Read inode ino into di; requires ext2fs_gdread() first. */
int
ext2fs_iget(struct ext2fsd *disk, ino_t ino, struct ext2fs_dinode *di)
{
	uint32_t bsize, boff, isize;
	uint64_t blk;
	char *buf;

	if (ext2fs_inoloc(disk, ino, &blk, &boff, &isize) == -1) {
		ERROR(disk, "invalid inode number");
		return (-1);
	}
	bsize = 1024u << le32toh(disk->d_fs.e2fs_log_bsize);
	buf = malloc(bsize);
	if (buf == NULL) {
		ERROR(disk, "out of memory");
		return (-1);
	}
	if (ext2fs_bread(disk, blk, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		ERROR(disk, "inode read failed");
		return (-1);
	}
	memcpy(di, buf + boff, isize < sizeof(*di) ? isize : sizeof(*di));
	free(buf);
	return (0);
}

/* Verify the metadata checksum of inode ino's on-disk slot. */
int
ext2fs_icsum(struct ext2fsd *disk, ino_t ino)
{
	uint32_t bsize, boff, isize;
	uint64_t blk;
	char *buf;
	int rc;

	if (ext2fs_inoloc(disk, ino, &blk, &boff, &isize) == -1) {
		ERROR(disk, "invalid inode number");
		return (-1);
	}
	bsize = 1024u << le32toh(disk->d_fs.e2fs_log_bsize);
	if ((buf = malloc(bsize)) == NULL) {
		ERROR(disk, "out of memory");
		return (-1);
	}
	if (ext2fs_bread(disk, blk, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		ERROR(disk, "inode read failed");
		return (-1);
	}
	rc = ext2_ei_csum_check(disk->d_csum_seed, ino,
	    le32toh(((const struct ext2fs_dinode *)(buf + boff))->e2di_gen),
	    (const struct ext2fs_dinode *)(buf + boff), isize);
	free(buf);
	return (rc);
}

/* Write di back to inode ino. */
int
ext2fs_iput(struct ext2fsd *disk, ino_t ino, const struct ext2fs_dinode *di)
{
	uint32_t bsize, boff, isize;
	uint64_t blk;
	char *buf;

	if (ext2fs_inoloc(disk, ino, &blk, &boff, &isize) == -1) {
		ERROR(disk, "invalid inode number");
		return (-1);
	}
	bsize = 1024u << le32toh(disk->d_fs.e2fs_log_bsize);
	buf = malloc(bsize);
	if (buf == NULL) {
		ERROR(disk, "out of memory");
		return (-1);
	}
	if (ext2fs_bread(disk, blk, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		ERROR(disk, "inode read failed");
		return (-1);
	}
	memcpy(buf + boff, di, isize < sizeof(*di) ? isize : sizeof(*di));
	if (le32toh(disk->d_fs.e2fs_features_rocompat) &
	    EXT2F_ROCOMPAT_METADATA_CKSUM) {
		/* The checksum spans the full on-disk inode slot. */
		const struct ext2fs_dinode *slot =
		    (const struct ext2fs_dinode *)(buf + boff);

		ext2_ei_csum_update(disk->d_csum_seed, ino,
		    le32toh(slot->e2di_gen),
		    (struct ext2fs_dinode *)(buf + boff), isize);
	}
	if (ext2fs_bwrite(disk, blk, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		ERROR(disk, "inode write failed");
		return (-1);
	}
	free(buf);
	return (0);
}
