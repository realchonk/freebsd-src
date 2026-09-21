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
static uint32_t ncleared;		/* 512-sectors cleared from dp */
static int badblk, dupblk;		/* per-inode bad/dup block count */

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

/*
 * Zero the block pointer that led to idesc->id_blkno: either a slot in
 * dp->e2di_blocks[], or 4 bytes inside the indirect block id_ptrblk.
 * Extent-tree pointers cannot be cleared in place; returns -1 then.
 */
static int
clearptr(struct inodesc *idesc)
{
	uint32_t bsize;
	char *buf;

	if (idesc->id_ptrblk == ~(uint64_t)0) {
		pwarn("EXTENT TREE BLOCK POINTERS NOT CLEARED (UNSUPPORTED)\n");
		uncorrected = 1;
		return (-1);
	}
	if (idesc->id_ptrblk == 0) {
		dp.e2di_blocks[idesc->id_ptridx] = 0;
		return (0);
	}
	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate clearptr buffer");
	if (ext2fs_bread(&disk, idesc->id_ptrblk, buf, bsize) != (ssize_t)bsize)
		pwarn("POINTER BLOCK %ju READ FAILED\n",
		    (uintmax_t)idesc->id_ptrblk);
	else {
		memset(buf + idesc->id_ptroff, 0, 4);
		if (ext2fs_bwrite(&disk, idesc->id_ptrblk, buf, bsize) !=
		    (ssize_t)bsize)
			pwarn("POINTER BLOCK %ju WRITE FAILED\n",
			    (uintmax_t)idesc->id_ptrblk);
	}
	free(buf);
	return (0);
}

/* Recount callback: tally the 512-sectors an inode still claims. */
static uint32_t nrecount;

static int
countblock(struct inodesc *idesc)
{

	if (idesc->id_blkno < (uint64_t)maxfsblock)
		nrecount += (1024u <<
		    le32toh(disk.d_fs.e2fs_log_bsize)) / 512;
	return (KEEPON);
}

/* pass1 block callback: mark the block used, detect duplicates. */
static int
pass1check(struct inodesc *idesc)
{
	uint64_t b = idesc->id_blkno;

	if (b >= (uint64_t)maxfsblock) {
		pwarn("BLOCK %ju OUT OF RANGE I=%ju\n", (uintmax_t)b,
		    (uintmax_t)idesc->id_ino);
		if (dofix(idesc, "CLEAR BAD BLOCK POINTER") != 0) {
			if (clearptr(idesc) != 0)
				return (SKIP);
			ncleared++;
			fsmodified = 1;
			return (ALTERED);
		}
		if (++badblk >= MAXBAD) {
			pwarn("EXCESSIVE BAD BLKS I=%ju",
			    (uintmax_t)idesc->id_ino);
			if (preen)
				printf(" (SKIPPING)\n");
			else if (reply("CONTINUE") == 0) {
				ckfini(0);
				exit(EEXIT);
			}
			uncorrected = 1;
			return (STOP);
		}
		return (SKIP);
	}
	if (testbmap(b)) {
		/*
		 * Duplicate: the first inode to claim the block keeps
		 * it, later pointers are cleared like bad ones.
		 */
		pwarn("DUP BLOCK %ju I=%ju\n", (uintmax_t)b,
		    (uintmax_t)idesc->id_ino);
		if (dofix(idesc, "CLEAR DUPLICATE BLOCK POINTER") != 0) {
			if (clearptr(idesc) != 0)
				return (SKIP);
			ncleared++;
			fsmodified = 1;
			return (ALTERED);
		}
		if (++dupblk >= MAXDUP) {
			pwarn("EXCESSIVE DUP BLKS I=%ju",
			    (uintmax_t)idesc->id_ino);
			if (preen)
				printf(" (SKIPPING)\n");
			else if (reply("CONTINUE") == 0) {
				ckfini(0);
				exit(EEXIT);
			}
			uncorrected = 1;
			return (STOP);
		}
		return (KEEPON);
	}
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
		ncleared = 0;
		badblk = dupblk = 0;
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
		idesc.id_fix = nflag ? IGNORE : DONTKNOW;
		ckinode(&dp, &idesc);

		/*
		 * Write back dp if bad pointers were cleared from it,
		 * with the block count of whatever remains claimed.
		 */
		if (ncleared != 0) {
			struct inodesc rdesc;

			nrecount = 0;
			memset(&rdesc, 0, sizeof(rdesc));
			rdesc.id_func = countblock;
			rdesc.id_type = ADDR;
			rdesc.id_fix = IGNORE;
			ckinode(&dp, &rdesc);
			dp.e2di_nblock = htole32(nrecount);
			if (ext2fs_iput(&disk, ino, &dp) != 0)
				pwarn("INODE %ju WRITE FAILED\n",
				    (uintmax_t)ino);

			/*
			 * A repaired slow symlink may have lost its target:
			 * with a size but no blocks it is invalid, so offer
			 * to clear it as e2fsck does.
			 */
			if ((le16toh(dp.e2di_mode) & S_IFMT) == S_IFLNK) {
				struct inodesc cdesc;

				memset(&cdesc, 0, sizeof(cdesc));
				clri(&cdesc, ino, "SYMLINK");
			}
		}
	}
}
