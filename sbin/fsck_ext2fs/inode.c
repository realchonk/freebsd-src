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
		return (-1);
	ec = ext2fs_iget(&disk, ino, di);
	if (ec == 0)
		return (0);
	printf("failed to read inode %d: %s\n", (int)ino, disk.d_error);
	return (ec);
}

/*
 * Recurse over an indirect block: depth 1 entries are data blocks.
 * The pointer to this block was located by the caller in id_ptrblk/
 * id_ptroff/id_ptridx; each level reads into its own buffer.
 */
static int
ckinode_indir(uint64_t blkno, int depth, struct inodesc *idesc)
{
	uint32_t bsize;
	char *buf;
	int i, n, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	n = bsize / 4;

	/* The indirect block itself is in use, too. */
	idesc->id_blkno = blkno;
	go = idesc->id_func(idesc);
	/* Cleared (ALTERED) or unfixed bad (SKIP): do not descend. */
	if (go & (STOP | SKIP | ALTERED))
		return (go);

	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate indirect block buffer");
	if (ext2fs_bread(&disk, blkno, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		return (KEEPON);
	}
	for (i = 0; i < n; i++) {
		uint64_t b = le32toh(((uint32_t *)buf)[i]);

		if (b == 0)
			continue;
		/* The pointer to the next block lives in this one. */
		idesc->id_ptrblk = blkno;
		idesc->id_ptroff = i * 4;
		idesc->id_ptridx = 0;
		if (depth == 1) {
			idesc->id_blkno = b;
			go = idesc->id_func(idesc);
		} else
			go = ckinode_indir(b, depth - 1, idesc);
		if (go & STOP) {
			free(buf);
			return (go);
		}
	}
	free(buf);
	return (KEEPON);
}

/* Directory block walker shared by dirscan(). */
static char *dirbuf;
static uint64_t dirbufblk = ~0u;
static void	dirblock_csum(ino_t, void *, uint32_t);
static uint32_t	dirgen(ino_t);

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
		if ((le32toh(disk.d_fs.e2fs_features_rocompat) &
		    EXT2F_ROCOMPAT_METADATA_CKSUM) != 0 &&
		    ext2_dirent_csum_check(disk.d_csum_seed, idesc->id_ino,
		    dirgen(idesc->id_ino), dirbuf, bsize) != 0) {
			pwarn("I=%ju: DIRECTORY BLOCK %ju CHECKSUM WRONG\n",
			    (uintmax_t)idesc->id_ino,
			    (uintmax_t)idesc->id_blkno);
			if (dofix(idesc, "FIX DIRECTORY CHECKSUM") != 0) {
				dirblock_csum(idesc->id_ino, dirbuf, bsize);
				if (ext2fs_bwrite(&disk, idesc->id_blkno,
				    dirbuf, bsize) == (ssize_t)bsize)
					fsmodified = 1;
			}
		}
	}
	dp = (struct ext2fs_direct_2 *)(dirbuf + idesc->id_loc);
	spaceleft = bsize - idesc->id_loc;
	if (dircheck(idesc, dp, spaceleft) == 0) {
		/* Entry is bad: drop the rest of its block. */
		pwarn("I=%ju: BAD DIRECTORY ENTRY IN BLOCK %ju AT %d\n",
		    (uintmax_t)idesc->id_ino, (uintmax_t)idesc->id_blkno,
		    idesc->id_loc);
		uncorrected = 1;
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

/*
 * Refresh the checksum tail of a modified directory block of directory
 * dir, so the caller can write it back.
 */
static void
dirblock_csum(ino_t dir, void *buf, uint32_t bsize)
{

	if ((le32toh(disk.d_fs.e2fs_features_rocompat) &
	    EXT2F_ROCOMPAT_METADATA_CKSUM) == 0)
		return;
	ext2_dirent_csum_update(disk.d_csum_seed, dir, dirgen(dir), buf,
	    bsize);
}

/* The generation of inode ino, 0 when unreadable. */
static uint32_t
dirgen(ino_t ino)
{
	struct ext2fs_dinode di;

	if (ext2fs_iget(&disk, ino, &di) != 0)
		return (0);
	return (le32toh(di.e2di_gen));
}

/*
 * Collect up to max physical data blocks of one extent-tree node and
 * its children, skipping the index blocks themselves; returns the
 * number collected into *np.
 */
static void
ext_blocks(char *node, uint64_t *blks, uint32_t max, uint32_t *np)
{
	struct ext4_extent_header *eh = (struct ext4_extent_header *)node;
	uint32_t bsize;
	char *buf;
	uint32_t i, j;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (le16toh(eh->eh_magic) != EXT4_EXT_MAGIC ||
	    le16toh(eh->eh_ecount) > le16toh(eh->eh_max))
		return;
	if (le16toh(eh->eh_depth) == 0) {
		struct ext4_extent *ex = (struct ext4_extent *)(node +
		    sizeof(*eh));

		for (i = 0; i < le16toh(eh->eh_ecount) && *np < max; i++) {
			uint64_t start = ((uint64_t)le16toh(ex[i].e_start_hi)
			    << 32) | le32toh(ex[i].e_start_lo);
			uint32_t len = le16toh(ex[i].e_len) &
			    (EXT_INIT_MAX_LEN - 1);

			for (j = 0; j < len && *np < max; j++)
				blks[(*np)++] = start + j;
		}
		return;
	}

	buf = malloc(bsize);
	if (buf == NULL)
		return;
	{
		struct ext4_extent_index *ix = (struct ext4_extent_index *)
		    (node + sizeof(*eh));

		for (i = 0; i < le16toh(eh->eh_ecount) && *np < max; i++) {
			uint64_t leaf = ((uint64_t)le16toh(ix[i].ei_leaf_hi)
			    << 32) | le32toh(ix[i].ei_leaf_lo);

			if (ext2fs_bread(&disk, leaf, buf, bsize) ==
			    (ssize_t)bsize)
				ext_blocks(buf, blks, max, np);
		}
	}
	free(buf);
}

/*
 * The first max physical data blocks of the directory di, following
 * either its block pointers or its extent tree.
 */
uint32_t
dir_blocks(const struct ext2fs_dinode *di, uint64_t *blks, uint32_t max)
{
	uint32_t i, n = 0;

	if (le32toh(di->e2di_flags) & EXT4_EXTENTS) {
		ext_blocks((char *)di->e2di_blocks, blks, max, &n);
		return (n);
	}
	for (i = 0; i < EXT2_NDIR_BLOCKS && n < max; i++)
		if (le32toh(di->e2di_blocks[i]) != 0)
			blks[n++] = le32toh(di->e2di_blocks[i]);
	return (n);
}

/*
 * Write the in-memory directory block holding the entry that idesc
 * just visited (and possibly modified) back to disk.
 */
int
direntry_write(struct inodesc *idesc)
{
	uint32_t bsize;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (dirbufblk != idesc->id_blkno)
		return (-1);
	dirblock_csum(idesc->id_ino, dirbuf, bsize);
	if (ext2fs_bwrite(&disk, idesc->id_blkno, dirbuf, bsize) !=
	    (ssize_t)bsize)
		return (-1);
	return (0);
}

/*
 * Insert an entry for target named name with filetype ftype into the
 * directory dirino, using free space in its direct blocks: either an
 * unused entry, or the slack of the last entry in a block.  Returns 0
 * on success.
 */
int
dir_add_entry(ino_t dirino, const char *name, int ftype, ino_t target)
{
	struct ext2fs_dinode di;
	struct ext2fs_direct_2 *dp;
	uint64_t b, blks[EXT2_NDIR_BLOCKS];
	uint32_t bsize, need, nl, reclen, avail, usable;
	uint32_t i, off, nblocks, slot = 0, lastoff;
	char *buf;
	int found = 0;

	nl = strlen(name);
	need = EXT2_DIR_REC_LEN(nl);
	if (ginode(dirino, &di) != 0)
		return (-1);
	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate directory buffer");
	nblocks = dir_blocks(&di, blks, EXT2_NDIR_BLOCKS);

	for (i = 0; i < nblocks && found == 0; i++) {
		b = blks[i];
		if (b == 0 || b >= (uint64_t)maxfsblock)
			continue;
		if (ext2fs_bread(&disk, b, buf, bsize) != (ssize_t)bsize)
			continue;
		/* Entries must leave room for the checksum tail. */
		usable = bsize;
		if ((le32toh(disk.d_fs.e2fs_features_rocompat) &
		    EXT2F_ROCOMPAT_METADATA_CKSUM) != 0 &&
		    ext2_dirent_has_tail(buf, bsize))
			usable = bsize - sizeof(struct ext2fs_direct_tail);
		lastoff = 0;
		for (off = 0; off + EXT2_DIR_REC_LEN(0) <= usable; ) {
			dp = (struct ext2fs_direct_2 *)(buf + off);
			reclen = le16toh(dp->e2d_reclen);
			if (reclen < EXT2_DIR_REC_LEN(0) ||
			    off + reclen > usable)
				break;
			if (le32toh(dp->e2d_ino) == 0 && reclen >= need) {
				/* Unused entry with room. */
				slot = off;
				avail = reclen;
				found = 1;
				break;
			}
			lastoff = off;
			off += reclen;
		}
		if (found != 0)
			break;
		/* Split the slack of the last entry if it reaches the end. */
		if (off == usable && lastoff + EXT2_DIR_REC_LEN(0) <= usable) {
			uint32_t lastlen;

			dp = (struct ext2fs_direct_2 *)(buf + lastoff);
			reclen = le16toh(dp->e2d_reclen);
			lastlen = EXT2_DIR_REC_LEN(dp->e2d_namlen);
			if (le32toh(dp->e2d_ino) != 0 &&
			    reclen - lastlen >= need) {
				slot = lastoff + lastlen;
				avail = reclen - lastlen;
				dp->e2d_reclen = htole16(lastlen);
				found = 1;
			}
		}
	}
	if (found == 0) {
		free(buf);
		return (-1);
	}

	dp = (struct ext2fs_direct_2 *)(buf + slot);
	dp->e2d_ino = htole32((uint32_t)target);
	dp->e2d_reclen = htole16(avail);
	dp->e2d_namlen = nl;
	dp->e2d_type = ftype;
	memset(dp->e2d_name, 0, avail - EXT2_DIR_REC_LEN(0));
	memcpy(dp->e2d_name, name, nl);
	dirblock_csum(dirino, buf, bsize);
	if (ext2fs_bwrite(&disk, b, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		return (-1);
	}
	free(buf);
	return (0);
}

/* Recurse over an indirect block in DATA mode; depth 1 is dirscan()ed. */
static int
ckinode_indir_data(uint64_t blkno, int depth, struct inodesc *idesc)
{
	uint32_t bsize;
	char *buf;
	int i, n, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	n = bsize / 4;
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate indirect block buffer");
	if (ext2fs_bread(&disk, blkno, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		return (KEEPON);
	}
	for (i = 0; i < n; i++) {
		uint64_t b = le32toh(((uint32_t *)buf)[i]);

		if (b == 0)
			continue;
		if (depth == 1) {
			idesc->id_blkno = b;
			go = dirscan(idesc);
		} else
			go = ckinode_indir_data(b, depth - 1, idesc);
		if (go & STOP) {
			free(buf);
			return (go);
		}
	}
	free(buf);
	return (KEEPON);
}

/* ADDR walk: id_func on every referenced block, indirects included. */
static int	ckinode_addr(struct ext2fs_dinode *, struct inodesc *);

/*
 * Walk one extent-tree node (on disk, or the root inside the inode's
 * block array): leaf extents map runs of data blocks, interior
 * entries point at further nodes.  ADDR walks visit every physical
 * block (index blocks included); DATA walks dirscan() each data block
 * in logical order.  Extent pointers cannot be cleared in place.
 */
static int
ckext_node(char *node, struct inodesc *idesc, uint32_t maxent)
{
	struct ext4_extent_header *eh = (struct ext4_extent_header *)node;
	uint32_t bsize;
	char *buf;
	int i, j, go;

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (le16toh(eh->eh_magic) != EXT4_EXT_MAGIC ||
	    le16toh(eh->eh_ecount) > le16toh(eh->eh_max) ||
	    le16toh(eh->eh_ecount) > maxent) {
		pwarn("I=%ju: BAD EXTENT TREE HEADER\n",
		    (uintmax_t)idesc->id_ino);
		uncorrected = 1;
		return (KEEPON);
	}
	if (le16toh(eh->eh_depth) == 0) {
		struct ext4_extent *ex = (struct ext4_extent *)(node +
		    sizeof(*eh));

		for (i = 0; i < le16toh(eh->eh_ecount); i++) {
			uint64_t start = ((uint64_t)le16toh(ex[i].e_start_hi)
			    << 32) | le32toh(ex[i].e_start_lo);
			uint32_t len = le16toh(ex[i].e_len) &
			    (EXT_INIT_MAX_LEN - 1);

			for (j = 0; j < (int)len; j++) {
				idesc->id_blkno = start + j;
				idesc->id_ptrblk = ~(uint64_t)0;
				if (idesc->id_type == DATA)
					go = dirscan(idesc);
				else
					go = idesc->id_func(idesc);
				if (go & STOP)
					return (go);
			}
		}
		return (KEEPON);
	} else {
		struct ext4_extent_index *ix = (struct ext4_extent_index *)
		    (node + sizeof(*eh));

		for (i = 0; i < le16toh(eh->eh_ecount); i++) {
			uint64_t leaf = ((uint64_t)le16toh(ix[i].ei_leaf_hi)
			    << 32) | le32toh(ix[i].ei_leaf_lo);

			/* The index block itself is allocated, too. */
			idesc->id_blkno = leaf;
			idesc->id_ptrblk = ~(uint64_t)0;
			if (idesc->id_type == ADDR) {
				go = idesc->id_func(idesc);
				if (go & STOP)
					return (go);
			}
			if ((buf = malloc(bsize)) == NULL)
				err(8, "cannot allocate extent buffer");
			if (ext2fs_bread(&disk, leaf, buf, bsize) !=
			    (ssize_t)bsize) {
				free(buf);
				continue;
			}
			go = ckext_node(buf, idesc,
			    (bsize - sizeof(*eh) - 4) / 12);
			free(buf);
			if (go & STOP)
				return (go);
		}
		return (KEEPON);
	}
}

/*
 * Walk the block pointers of dp: with id_type ADDR, call id_func for
 * each block; with DATA, dirscan() each block in file order.  Follows
 * the single, double, and triple indirect blocks.
 */
int
ckinode(struct ext2fs_dinode *dp, struct inodesc *idesc)
{
	mode_t mode;
	int i, go;

	if (idesc->id_fix != IGNORE)
		idesc->id_fix = DONTKNOW;
	mode = le16toh(dp->e2di_mode) & S_IFMT;
	/*
	 * Device nodes carry their number in the block array, and fast
	 * symlinks their target; neither has blocks to walk.
	 */
	if (mode == S_IFBLK || mode == S_IFCHR ||
	    (mode == S_IFLNK && le32toh(dp->e2di_size) < EXT2_MAXSYMLINKLEN))
		return (KEEPON);
	if (le32toh(dp->e2di_flags) & EXT4_EXTENTS)
		return (ckext_node((char *)dp->e2di_blocks, idesc, 4));
	if (idesc->id_type == ADDR || (mode != S_IFDIR && mode != S_IFREG))
		return (ckinode_addr(dp, idesc));

	/* DATA walk of a directory or regular file. */
	for (i = 0; i < EXT2_N_BLOCKS; i++) {
		uint64_t b = le32toh(dp->e2di_blocks[i]);

		if (b == 0)
			continue;
		if (i < EXT2_NDIR_BLOCKS) {
			idesc->id_blkno = b;
			go = dirscan(idesc);
		} else
			go = ckinode_indir_data(b, i - EXT2_IND_BLOCK + 1,
			    idesc);
		if (go & STOP)
			break;
	}
	return (KEEPON);
}

/* ADDR walk: id_func on every referenced block, indirects included. */
static int
ckinode_addr(struct ext2fs_dinode *dp, struct inodesc *idesc)
{
	int i, go;

	for (i = 0; i < EXT2_N_BLOCKS; i++) {
		uint64_t b = le32toh(dp->e2di_blocks[i]);

		if (b == 0)
			continue;
		/* Pointers at this level live in the inode itself. */
		idesc->id_ptrblk = 0;
		idesc->id_ptroff = 0;
		idesc->id_ptridx = i;
		if (i < EXT2_NDIR_BLOCKS) {
			idesc->id_blkno = b;
			go = idesc->id_func(idesc);
		} else
			go = ckinode_indir(b, i - EXT2_IND_BLOCK + 1, idesc);
		if (go & STOP)
			break;
	}
	return (KEEPON);
}
