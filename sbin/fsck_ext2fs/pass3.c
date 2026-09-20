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

static struct inodesc idesc;
static ino_t *dstack;
static size_t dsp;

/* pass3 callback: enqueue directory entries not yet reached. */
static int
pass3check(struct inodesc *idp)
{
	struct ext2fs_direct_2 *dp = idp->id_dirp;
	ino_t ino = le32toh(dp->e2d_ino);

	if (ino != 0 && ino <= (ino_t)maxino &&
	    getinostat(ino)->ino_state == DSTATE) {
		getinostat(ino)->ino_state = DFOUND;
		dstack[dsp++] = ino;
	}
	return (KEEPON);
}

/*
 * Check directory connectivity: mark every directory reachable from
 * the root by walking directory entries; the remainder are orphans,
 * candidates for reattachment in lost+found.
 */
void
pass3(void)
{
	ino_t ino;

	dstack = calloc(maxino + 1, sizeof(*dstack));
	if (dstack == NULL)
		err(8, "cannot allocate directory stack");
	memset(&idesc, 0, sizeof(idesc));
	idesc.id_type = DATA;
	idesc.id_func = pass3check;

	if (getinostat(EXT2_ROOTINO)->ino_state == DSTATE) {
		getinostat(EXT2_ROOTINO)->ino_state = DFOUND;
		dstack[dsp++] = EXT2_ROOTINO;
	}
	while (dsp > 0) {
		struct ext2fs_dinode di;

		ino = dstack[--dsp];
		if (ginode(ino, &di) != 0)
			continue;
		idesc.id_ino = ino;
		idesc.id_entryno = 0;
		idesc.id_firstblock = le32toh(di.e2di_blocks[0]);
		idesc.id_filesize = le32toh(di.e2di_size);
		ckinode(&di, &idesc);
	}
	free(dstack);
	dstack = NULL;

	for (ino = EXT2_ROOTINO + 1; ino <= (ino_t)maxino; ino++)
		if (getinostat(ino)->ino_state == DSTATE)
			pwarn("UNREF DIR I=%ju\n", (uintmax_t)ino);
}
