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

/* pass4 callback: unclaim the blocks of an inode being cleared. */
static int
freeblock(struct inodesc *idesc)
{
	uint64_t b = idesc->id_blkno;

	if (b < (uint64_t)maxfsblock && testbmap(b)) {
		clrbmap(b);
		n_blks--;
	}
	return (KEEPON);
}

/*
 * Clear inode ino: unclaim its blocks, wipe it, and mark it free.
 */
void
clri(struct inodesc *idesc, ino_t ino, const char *type)
{
	struct ext2fs_dinode di;
	struct inodesc fdesc;

	(void)idesc;
	pwarn("UNREF %s I=%ju\n", type, (uintmax_t)ino);
	if (preen || reply("CLEAR") == 1) {
		if (preen)
			printf(" (CLEARED)\n");
		if (ginode(ino, &di) == 0) {
			memset(&fdesc, 0, sizeof(fdesc));
			fdesc.id_func = freeblock;
			fdesc.id_type = ADDR;
			fdesc.id_fix = IGNORE;
			ckinode(&di, &fdesc);
			memset(&di, 0, sizeof(di));
			if (ext2fs_iput(&disk, ino, &di) == 0)
				fsmodified = 1;
			else
				pwarn("INODE %ju WRITE FAILED\n",
				    (uintmax_t)ino);
		}
		getinostat(ino)->ino_state = USTATE;
		n_files--;
	} else
		uncorrected = 1;
}

/*
 * After pass2 counted every directory reference, compare the stored
 * link count of each allocated inode with the references found: with
 * none, the inode is unattached; with a mismatch, the count is wrong.
 */
void
pass4(void)
{
	struct ext2fs_dinode di;
	struct inostat *stp;
	struct inodesc idesc;
	ino_t ino;
	uint32_t stored, refs, firstino, orphanino;
	int residual;
	const char *type;

	memset(&idesc, 0, sizeof(idesc));
	idesc.id_fix = nflag ? IGNORE : DONTKNOW;
	firstino = le32toh(disk.d_fs.e2fs_rev) == E2FS_REV0 ? EXT2_FIRSTINO :
	    le32toh(disk.d_fs.e2fs_first_ino);
	/*
	 * The orphan-file inode is referenced by the superblock alone;
	 * its number is not modelled by struct ext2fs (sb offset 0x280).
	 */
	orphanino = 0;
	if (le32toh(disk.d_fs.e2fs_features_compat) & 0x1000)
		memcpy(&orphanino, disk.d_sbunion.d_sb + 0x280, 4);
	orphanino = le32toh(orphanino);		/* ORPHAN_FILE feature */

	for (ino = EXT2_ROOTINO; ino <= (ino_t)maxino; ino++) {
		/*
		 * Reserved inodes (journal, resize, ...) are referenced by
		 * no directory; only the root participates in link counts.
		 */
		if ((ino < firstino && ino != EXT2_ROOTINO) || ino == orphanino)
			continue;
		stp = getinostat(ino);
		switch (stp->ino_state) {
		case FSTATE:
		case DFOUND:
			if (ginode(ino, &di) != 0)
				break;
			stored = le16toh(di.e2di_nlink);
			residual = stp->ino_linkcnt;
			refs = (uint32_t)((int)stored - residual);
			type = stp->ino_state == DFOUND ? "DIR" : "FILE";
			if (refs == 0) {
				clri(&idesc, ino, type);
				break;
			}
			if (residual != 0) {
				pwarn("LINK COUNT %s I=%ju (COUNT %u should "
				    "be %u)\n", type, (uintmax_t)ino, stored,
				    refs);
				if (dofix(&idesc, "ADJUST LINK COUNT") != 0) {
					di.e2di_nlink =
					    htole16((uint16_t)refs);
					if (ext2fs_iput(&disk, ino, &di) != 0)
						pwarn("INODE %ju WRITE "
						    "FAILED\n", (uintmax_t)ino);
					else
						fsmodified = 1;
				}
			}
			break;
		case DSTATE:
			/* Unreached directory: pass3 offered to fix it. */
			pwarn("UNREF DIR I=%ju\n", (uintmax_t)ino);
			uncorrected = 1;
			break;
		default:
			break;
		}
	}
}
