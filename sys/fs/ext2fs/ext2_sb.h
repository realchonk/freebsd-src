/*-
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Benjamin Stürz <benni@stuerz.xyz>
 * under sponsorship from the FreeBSD Foundation.
 */

#ifndef _FS_EXT2FS_EXT2_SB_H_
#define _FS_EXT2FS_EXT2_SB_H_

#include <sys/types.h>		/* off_t */

struct ext2fs;
struct malloc_type;

/*
 * Read and validate an ext2/3/4 superblock.
 *
 * This is dual-mode code, compiled into both the kernel ext2fs driver and
 * lib/libext2fs (mirroring the ffs_sbget()/libufs arrangement).  "readfunc"
 * performs the actual I/O -- bread(9) in the kernel, pread(2) in userland --
 * and is responsible for allocating the buffer returned in *fsp.  On success
 * the caller owns the buffer and must free it; "filltype" gives the malloc
 * type to use when ext2_sbget() frees the buffer on the bad-magic error path
 * (it is ignored in userland).  Validation is limited to the ext2 magic
 * number; mount-time feature checks are left to the caller.
 */
int	ext2_sbget(void *devfd, struct ext2fs **fsp, off_t sblockloc,
	    int flags, struct malloc_type *filltype,
	    int (*readfunc)(void *, off_t, void **, int));

#endif /* !_FS_EXT2FS_EXT2_SB_H_ */
