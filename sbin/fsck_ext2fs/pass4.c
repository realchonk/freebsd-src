/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/stat.h>

#include "fsck_ext2fs.h"

/*
 * After pass2 counted every directory reference, compare the stored
 * link count of each allocated inode with the references found.
 */
void
pass4(void)
{
	struct ext2fs_dinode di;
	struct inostat *stp;
	ino_t ino;
	int n;

	for (ino = EXT2_ROOTINO; ino <= (ino_t)maxino; ino++) {
		stp = getinostat(ino);
		switch (stp->ino_state) {
		case FSTATE:
		case DFOUND:
			n = stp->ino_linkcnt;
			if (n != 0) {
				ginode(ino, &di);
				pwarn("LINK COUNT %s I=%ju (COUNT %u should "
				    "be %u)\n",
				    stp->ino_state == FSTATE ? "FILE" : "DIR",
				    (uintmax_t)ino, le16toh(di.e2di_nlink),
				    (unsigned)(le16toh(di.e2di_nlink) - n));
			}
			break;
		default:
			/* DSTATE orphans are pass3's to report. */
			break;
		}
	}
}
