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

/* Read inode ino. */
int
ginode(ino_t ino, struct ext2fs_dinode *di)
{
	int ec;
	if (ino == 0 || ino > (ino_t)maxino)
		return (0);
	ec = ext2fs_iget(&disk, ino, di);
	if (ec == 0)
		return (0);
	printf ("failed to read inode %d: %s\n", (int)ino, disk.d_error);
	return ec;
}

/* Recurse over an indirect block: depth 1 entries are data blocks. */
static int
ckinode_indir(uint64_t blkno, int depth, struct inodesc *idesc, char *buf)
{
	uint32_t bsize;
	int i, n, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	n = bsize / 4;

	/* The indirect block itself is in use, too. */
	idesc->id_blkno = blkno;
	go = idesc->id_func(idesc);
	if (go & STOP)
		return (go);

	if (ext2fs_bread(&disk, blkno, buf, bsize) != (ssize_t)bsize)
		return (KEEPON);
	for (i = 0; i < n; i++) {
		uint64_t b = le32toh(((uint32_t *)buf)[i]);

		if (b == 0)
			continue;
		if (depth == 1) {
			idesc->id_blkno = b;
			go = idesc->id_func(idesc);
			if (go & STOP)
				return (go);
		} else {
			go = ckinode_indir(b, depth - 1, idesc, buf);
			if (go & STOP)
				return (go);
		}
	}
	return (KEEPON);
}

/* Directory block walker shared by dirscan(). */
static char *dirbuf;
static uint64_t dirbufblk = ~0u;

/*
 * Validate the directory entry dp that has spaceleft bytes to the end
 * of its block; nonzero if it is well formed.
 */
static int
dircheck(struct inodesc *idesc, struct ext2fs_direct_2 *dp,
    uint32_t spaceleft)
{
	uint16_t reclen;
	uint8_t namlen;
	size_t i;

	reclen = le16toh(dp->e2d_reclen);
	namlen = dp->e2d_namlen;
	if (reclen < EXT2_DIR_REC_LEN(0) || (reclen & EXT2_DIR_ROUND) != 0 ||
	    reclen > spaceleft || reclen < EXT2_DIR_REC_LEN(namlen) ||
	    idesc->id_filesize < reclen)
		return (0);
	if (le32toh(dp->e2d_ino) == 0)
		return (1);
	if (namlen == 0)
		return (0);
	for (i = 0; i < namlen; i++)
		if (dp->e2d_name[i] == '\0' || dp->e2d_name[i] == '/')
			return (0);
	return (1);
}

/*
 * Return the next valid entry in the directory block idesc->id_blkno,
 * or NULL at its end.  Advances id_loc/id_filesize past the entry; a
 * malformed entry terminates the rest of its block.
 */
static struct ext2fs_direct_2 *
fsck_readdir(struct inodesc *idesc)
{
	struct ext2fs_direct_2 *dp;
	uint32_t bsize, spaceleft;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (idesc->id_filesize <= 0 || idesc->id_loc >= (int)bsize)
		return (NULL);
	if (dirbuf == NULL) {
		dirbuf = malloc(bsize);
		if (dirbuf == NULL)
			err(8, "cannot allocate directory buffer");
	}
	if (dirbufblk != idesc->id_blkno) {
		if (ext2fs_bread(&disk, idesc->id_blkno, dirbuf, bsize) !=
		    (ssize_t)bsize)
			return (NULL);
		dirbufblk = idesc->id_blkno;
	}
	dp = (struct ext2fs_direct_2 *)(dirbuf + idesc->id_loc);
	spaceleft = bsize - idesc->id_loc;
	if (dircheck(idesc, dp, spaceleft) == 0) {
		/* Entry is bad: drop the rest of its block. */
		idesc->id_filesize -= spaceleft;
		idesc->id_loc = bsize;
		return (NULL);
	}
	idesc->id_loc += le16toh(dp->e2d_reclen);
	idesc->id_filesize -= le16toh(dp->e2d_reclen);
	return (dp);
}

/*
 * Walk the entries of the directory block idesc->id_blkno via id_func.
 */
static int
dirscan(struct inodesc *idesc)
{
	struct ext2fs_direct_2 *dp;
	uint32_t bsize;
	int go;

	if (idesc->id_type != DATA)
		errx(8, "wrong type to dirscan %d", idesc->id_type);
	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (idesc->id_blkno >= (uint64_t)maxfsblock) {
		idesc->id_filesize -= bsize;
		return (SKIP);
	}
	idesc->id_loc = 0;
	for (dp = fsck_readdir(idesc); dp != NULL; dp = fsck_readdir(idesc)) {
		idesc->id_dirp = dp;
		go = idesc->id_func(idesc);
		if (go & STOP)
			return (go);
	}
	return (idesc->id_filesize > 0 ? KEEPON : STOP);
}

/* Recurse over an indirect block in DATA mode; depth 1 is dirscan()ed. */
static int
ckinode_indir_data(uint64_t blkno, int depth, struct inodesc *idesc,
    char *buf)
{
	uint32_t bsize;
	int i, n, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	n = bsize / 4;
	if (ext2fs_bread(&disk, blkno, buf, bsize) != (ssize_t)bsize)
		return (KEEPON);
	for (i = 0; i < n; i++) {
		uint64_t b = le32toh(((uint32_t *)buf)[i]);

		if (b == 0)
			continue;
		if (depth == 1) {
			idesc->id_blkno = b;
			go = dirscan(idesc);
			if (go & STOP)
				return (go);
		} else {
			go = ckinode_indir_data(b, depth - 1, idesc, buf);
			if (go & STOP)
				return (go);
		}
	}
	return (KEEPON);
}

/* ADDR walk: id_func on every referenced block, indirects included. */
static int	ckinode_addr(struct ext2fs_dinode *, struct inodesc *);

/*
 * Walk the block pointers of dp: with id_type ADDR, call id_func for
 * each block; with DATA, dirscan() each block in file order.  Follows
 * the single, double, and triple indirect blocks.
 */
int
ckinode(struct ext2fs_dinode *dp, struct inodesc *idesc)
{
	mode_t mode;
	uint32_t bsize;
	char *buf;
	int i, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	mode = le16toh(dp->e2di_mode) & S_IFMT;
	if (idesc->id_type == ADDR || (mode != S_IFDIR && mode != S_IFREG))
		return (ckinode_addr(dp, idesc));

	/* DATA walk of a directory or regular file. */
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate indirect block buffer");
	for (i = 0; i < EXT2_N_BLOCKS; i++) {
		uint64_t b = le32toh(dp->e2di_blocks[i]);

		if (b == 0)
			continue;
		if (i < EXT2_NDIR_BLOCKS) {
			idesc->id_blkno = b;
			go = dirscan(idesc);
		} else
			go = ckinode_indir_data(b, i - EXT2_IND_BLOCK + 1,
			    idesc, buf);
		if (go & STOP)
			break;
	}
	free(buf);
	return (KEEPON);
}

static int
ckinode_addr(struct ext2fs_dinode *dp, struct inodesc *idesc)
{
	uint32_t bsize;
	char *buf;
	int i, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate indirect block buffer");
	for (i = 0; i < EXT2_N_BLOCKS; i++) {
		uint64_t b = le32toh(dp->e2di_blocks[i]);

		if (b == 0)
			continue;
		if (i < EXT2_NDIR_BLOCKS) {
			idesc->id_blkno = b;
			go = idesc->id_func(idesc);
		} else
			go = ckinode_indir(b, i - EXT2_IND_BLOCK + 1, idesc,
			    buf);
		if (go & STOP)
			break;
	}
	free(buf);
	return (KEEPON);
}
