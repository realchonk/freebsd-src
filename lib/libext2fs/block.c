/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/types.h>
#include <sys/endian.h>

#include <unistd.h>

#include <libext2fs.h>

/*
 * Read "size" bytes starting at filesystem block "blkno" into "buf".
 */
ssize_t
ext2fs_bread(struct ext2fsd *disk, uint64_t blkno, void *buf, size_t size)
{
	struct ext2fs *fs;
	uint32_t bsize;
	off_t off;

	fs = &disk->d_fs;
	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	off = (off_t)blkno * bsize;
	return (pread(disk->d_fd, buf, size, off));
}
