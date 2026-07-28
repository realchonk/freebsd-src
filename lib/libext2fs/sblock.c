/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libext2fs.h>

static int handle_disk_read(struct ext2fsd *, struct ext2fs *, int);

/*
 * Read the standard superblock.
 *
 * The following option flags can be or'ed into disk->d_lookupflags:
 *
 * UFS_NOMSG indicates that superblock inconsistency error messages
 *    should not be printed.
 *
 * UFS_NOCSUM causes only the superblock itself to be returned, but does
 *    not read in any auxillary data structures like the cylinder group
 *    summary information.
 */
int
sbread(struct ext2fsd *disk)
{
	struct ext2fs *fs;
	int error;

	error = ext2fs_sbget(disk->d_fd, &fs, disk->d_sblockloc, disk->d_lookupflags);
	return handle_disk_read(disk, fs, error);
}

static int
handle_disk_read(struct ext2fsd *disk, struct ext2fs *fs, int error)
{

	ERROR(disk, NULL);
	if (error != 0) {
		switch (error) {
		case EIO:
			ERROR(disk, "non-existent or truncated superblock");
			break;
		case ENOENT:
			ERROR(disk, "no usable known superblock found");
			break;
		case EINTEGRITY:
			ERROR(disk, "superblock check-hash failure");
			break;
		case ENOSPC:
			ERROR(disk, "failed to allocate space for superblock "
			    "information");
			break;
		case EINVAL:
			ERROR(disk, "The previous newfs operation on this "
			    "volume did not complete.\nYou must complete "
			    "newfs before using this volume.");
			break;
		default:
			ERROR(disk, "unknown superblock read error");
			errno = EIO;
			break;
		}
		disk->d_version = 0;
		return (-1);
	}
	memcpy(&disk->d_fs, fs, SBLOCKSIZE);
	free(fs);
	fs = &disk->d_fs;

	if (fs->fs_magic == E2FS_MAGIC)
		disk->d_version = 2;

	/* TODO: detect ext3 & ext4 */

	disk->d_bsize = fs->fs_fsize / fsbtodb(fs, 1);
	disk->d_sblock = fs->fs_sblockloc / disk->d_bsize;
	disk->d_si = fs->fs_si;
	return (0);
}
