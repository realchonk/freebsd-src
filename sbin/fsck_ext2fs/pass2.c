/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/stat.h>

#include <stdlib.h>
#include <string.h>

#include "fsck_ext2fs.h"

/*
 * pass2 callback: validate one directory entry of the directory being
 * scanned and account its reference against the target inode.
 */
static int
pass2check(struct inodesc *idp)
{
	struct ext2fs_direct_2 *dp = idp->id_dirp;
	struct inostat *stp;
	ino_t ino;

	ino = le32toh(dp->e2d_ino);
	if (ino == 0)
		return (KEEPON);
	if (ino > (ino_t)maxino || getinostat(ino)->ino_state == USTATE) {
		pwarn("I=%ju: ENTRY %.*s HAS BAD INODE %ju\n",
		    (uintmax_t)idp->id_ino, dp->e2d_namlen, dp->e2d_name,
		    (uintmax_t)ino);
		if (dofix(idp, "CLEAR ENTRY") != 0) {
			dp->e2d_ino = 0;
			if (direntry_write(idp) == 0)
				fsmodified = 1;
			else
				pwarn("DIRECTORY BLOCK %ju WRITE FAILED\n",
				    (uintmax_t)idp->id_blkno);
		}
		idp->id_entryno++;
		return (KEEPON);
	}
	stp = getinostat(ino);
	stp->ino_linkcnt--;

	/* '.' must be the first entry and point at the directory itself. */
	if (idp->id_firstblock == idp->id_blkno && idp->id_entryno == 0) {
		if (dp->e2d_namlen != 1 || dp->e2d_name[0] != '.') {
			pwarn("I=%ju: FIRST ENTRY %.*s IS NOT '.'\n",
			    (uintmax_t)idp->id_ino, dp->e2d_namlen,
			    dp->e2d_name);
			uncorrected = 1;
		} else if (ino != idp->id_ino) {
			pwarn("I=%ju: BAD '.' INO %ju\n",
			    (uintmax_t)idp->id_ino, (uintmax_t)ino);
			if (dofix(idp, "FIX '.' ENTRY") != 0) {
				stp->ino_linkcnt++;
				getinostat(idp->id_ino)->ino_linkcnt--;
				dp->e2d_ino = htole32((uint32_t)idp->id_ino);
				if (direntry_write(idp) == 0)
					fsmodified = 1;
				/* Checks below use the fixed target. */
				ino = idp->id_ino;
				stp = getinostat(ino);
			}
		}
	}
	/* Record which directory contains this one, for the '..' check.
	 * '.' and '..' do not count: a directory is its own '.' target
	 * and its parent's '..' target, neither establishes containment. */
	if ((stp->ino_state == DSTATE || stp->ino_state == DFOUND) &&
	    parentof[ino] == 0 && ino != idp->id_ino &&
	    !(dp->e2d_namlen == 2 && dp->e2d_name[0] == '.' &&
	    dp->e2d_name[1] == '.'))
		parentof[ino] = idp->id_ino;

	if (le32toh(disk.d_fs.e2fs_features_incompat) & EXT2F_INCOMPAT_FTYPE) {
		/* '.' and '..' are directories by definition. */
		int want = stp->ino_ftype;

		if ((dp->e2d_namlen == 1 && dp->e2d_name[0] == '.') ||
		    (dp->e2d_namlen == 2 && dp->e2d_name[0] == '.' &&
		    dp->e2d_name[1] == '.'))
			want = EXT2_FT_DIR;
		if (want != dp->e2d_type) {
			pwarn("I=%ju: ENTRY %.*s WRONG TYPE (ino %ju)\n",
			    (uintmax_t)idp->id_ino, dp->e2d_namlen,
			    dp->e2d_name, (uintmax_t)ino);
			if (dofix(idp, "FIX FILETYPE") != 0) {
				dp->e2d_type = want;
				if (direntry_write(idp) == 0)
					fsmodified = 1;
			}
		}
	}
	idp->id_entryno++;
	return (KEEPON);
}

/*
 * Validate the '..' entry of directory ino against the parent recorded
 * in the first sweep.  '.' and '..' always lead the first data block.
 */
static void
checkdotdot(ino_t ino)
{
	struct ext2fs_dinode di;
	struct ext2fs_direct_2 *dp;
	struct inodesc idesc;
	uint32_t bsize;
	uint64_t blk;
	char *buf;
	off_t off;

	if (ginode(ino, &di) != 0)
		return;
	if (parentof[ino] == 0) {
		/* No entry references this directory: pass3's orphan. */
		return;
	}
	uint64_t blk1[1];

	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (dir_blocks(&di, blk1, 1) != 1 || blk1[0] >= (uint64_t)maxfsblock)
		return;
	blk = blk1[0];
	buf = malloc(bsize);
	if (buf == NULL)
		return;
	if (ext2fs_bread(&disk, blk, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		return;
	}

	off = 0;
	dp = (struct ext2fs_direct_2 *)buf;
	if (le16toh(dp->e2d_reclen) >= EXT2_DIR_REC_LEN(1) &&
	    le32toh(dp->e2d_ino) == ino)
		off = le16toh(dp->e2d_reclen);
	dp = (struct ext2fs_direct_2 *)(buf + off);
	if (le16toh(dp->e2d_reclen) < EXT2_DIR_REC_LEN(2) ||
	    off + le16toh(dp->e2d_reclen) > bsize || dp->e2d_namlen != 2 ||
	    dp->e2d_name[0] != '.' || dp->e2d_name[1] != '.') {
		pwarn("I=%ju: SECOND ENTRY IS NOT '..'\n", (uintmax_t)ino);
		uncorrected = 1;
	} else if (le32toh(dp->e2d_ino) != parentof[ino]) {
		ino_t bad = le32toh(dp->e2d_ino);

		pwarn("I=%ju: BAD '..' INO %ju (parent %ju)\n", (uintmax_t)ino,
		    (uintmax_t)bad, (uintmax_t)parentof[ino]);
		memset(&idesc, 0, sizeof(idesc));
		idesc.id_fix = nflag ? IGNORE : DONTKNOW;
		if (dofix(&idesc, "FIX '..' ENTRY") != 0) {
			/* Undo the phantom reference, if '..' had one. */
			if (bad != 0 && bad <= (ino_t)maxino)
				getinostat(bad)->ino_linkcnt++;
			getinostat(parentof[ino])->ino_linkcnt--;
			dp->e2d_ino = htole32((uint32_t)parentof[ino]);
			if (ext2fs_bwrite(&disk, blk, buf, bsize) ==
			    (ssize_t)bsize)
				fsmodified = 1;
		}
	}
	free(buf);
}

/* pass2 state. */
static struct inodesc idesc;

/*
 * Check all directory inodes: walk each directory's entries, verify
 * '.'/'..', entry targets, and filetype bytes, and count references.
 */
void
pass2(void)
{
	ino_t ino;

	memset(&idesc, 0, sizeof(idesc));
	idesc.id_type = DATA;
	idesc.id_func = pass2check;
	idesc.id_fix = nflag ? IGNORE : DONTKNOW;

	for (ino = EXT2_ROOTINO; ino <= (ino_t)maxino; ino++) {
		struct ext2fs_dinode di;
		struct inostat *stp;
		uint64_t blk1[1];

		stp = getinostat(ino);
		if (stp->ino_state != DSTATE && stp->ino_state != DFOUND)
			continue;
		if (ginode(ino, &di) != 0)
			continue;
		idesc.id_ino = ino;
		idesc.id_entryno = 0;
		if (dir_blocks(&di, blk1, 1) == 1)
			idesc.id_firstblock = blk1[0];
		else
			idesc.id_firstblock = 0;
		idesc.id_filesize = le32toh(di.e2di_size);
		ckinode(&di, &idesc);
	}

	/* Second sweep: '..' once every parent is known. */
	for (ino = EXT2_ROOTINO + 1; ino <= (ino_t)maxino; ino++) {
		struct inostat *stp = getinostat(ino);

		if (stp->ino_state == DFOUND || stp->ino_state == DSTATE)
			checkdotdot(ino);
	}
}
