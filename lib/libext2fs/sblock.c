/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/endian.h>
#include <sys/types.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
ext2fs_sbread(struct ext2fsd *disk)
{
	struct ext2fs *fs;
	int error;

	error = ext2fs_sbget(disk->d_fd, &fs, disk->d_sblockloc,
	    disk->d_lookupflags);
	return (handle_disk_read(disk, fs, error));
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

	if (le16toh(fs->e2fs_magic) == E2FS_MAGIC) {
		/*
		 * There is no authoritative on-disk "ext version" field; the
		 * filesystem family is inferred from the feature flags, as
		 * e2fsprogs does: extents/64bit imply ext4, a journal implies
		 * ext3, otherwise it is plain ext2.
		 */
		if (le32toh(fs->e2fs_features_incompat) &
		    (EXT2F_INCOMPAT_EXTENTS | EXT2F_INCOMPAT_64BIT))
			disk->d_version = 4;
		else if (le32toh(fs->e2fs_features_compat) &
		    EXT2F_COMPAT_HASJOURNAL)
			disk->d_version = 3;
		else
			disk->d_version = 2;
	}

	return (0);
}

/*
 * Low-level superblock read for userland consumers.
 *
 * Reads the ext2 superblock from byte offset `sblockloc` (normally
 * SBLOCKOFFSET, 1024) on the device referred to by `devfd`, allocates a
 * buffer holding it and returns it in *fsp.  Returns 0 on success or one
 * of EIO (short read), ENOENT (bad magic) or ENOSPC (allocation failure).
 */
int
ext2fs_sbget(int devfd, struct ext2fs **fsp, off_t sblockloc, int flags)
{
	struct ext2fs *fs;

	(void)flags;

	fs = malloc(SBLOCKSIZE);
	if (fs == NULL)
		return (ENOSPC);

	if (pread(devfd, fs, SBLOCKSIZE, sblockloc) != SBLOCKSIZE) {
		free(fs);
		return (EIO);
	}

	if (le16toh(fs->e2fs_magic) != E2FS_MAGIC) {
		free(fs);
		return (ENOENT);
	}

	*fsp = fs;
	return (0);
}
