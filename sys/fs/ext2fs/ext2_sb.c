/*-
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

#include "ext2_sb.h"

#ifndef _KERNEL
/*
 * Userland (libext2fs) build.  Use the plain libc allocator; the malloc
 * type passed by callers is irrelevant here and is ignored by the macros.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2fs.h>

struct malloc_type;
#define	EXT2_MALLOC(size, type, flags)	malloc(size)
#define	EXT2_FREE(ptr, type)		free(ptr)

#else /* _KERNEL */
/*
 * Kernel build.  The caller supplies the malloc type (e.g. M_EXT2MNT) via
 * the filltype argument so that this file need not depend on a specific
 * malloc type being visible here.
 */
#include <sys/malloc.h>
#include <sys/systm.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2fs.h>

#define	EXT2_MALLOC(size, type, flags)	malloc((size), (type), (flags))
#define	EXT2_FREE(ptr, type)		free((ptr), (type))
#endif /* _KERNEL */

/*
 * Read and validate an ext2/3/4 superblock.
 *
 * Reads SBLOCKSIZE bytes from byte offset "sblockloc" (normally
 * SBLOCKOFFSET, 1024) on the device described by "devfd".  The read is
 * performed by "readfunc", which is responsible for allocating the buffer
 * and returning it in *bufp; on success the caller owns the buffer and must
 * release it with EXT2_FREE(*fsp, filltype).  Validation is limited to the
 * ext2 magic number; mount-time feature checks are intentionally left to
 * the caller, as a dumpfs-style tool may legitimately inspect filesystems
 * whose features the kernel refuses to mount.
 *
 * Returns 0 on success or an errno on failure: whatever "readfunc" returns
 * for an I/O failure (typically EIO), ENOENT if the magic number is absent,
 * or ENOSPC if the buffer cannot be allocated.
 */
int
ext2_sbget(void *devfd, struct ext2fs **fsp, off_t sblockloc, int flags,
    struct malloc_type *filltype,
    int (*readfunc)(void *devfd, off_t loc, void **bufp, int size))
{
	struct ext2fs *fs;
	int error;

	(void)flags;
	*fsp = NULL;

	error = readfunc(devfd, sblockloc, (void **)fsp, SBLOCKSIZE);
	if (error != 0)
		return (error);
	fs = *fsp;

	if (le16toh(fs->e2fs_magic) != E2FS_MAGIC) {
		EXT2_FREE(fs, filltype);
		*fsp = NULL;
		return (ENOENT);
	}
	return (0);
}
