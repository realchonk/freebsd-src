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

/* Dual-mode superblock reader shared with the kernel ext2fs driver. */
struct malloc_type;
int	ext2_sbget(void *, struct ext2fs **, off_t, int, struct malloc_type *,
	    int (*)(void *, off_t, void **, int));
static int	ext2_use_pread(void *, off_t, void **, int);

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
 * Thin wrapper around the dual-mode ext2_sbget() shared with the kernel
 * ext2fs driver, supplying the userland I/O backend (ext2_use_pread).
 */
int
ext2fs_sbget(int devfd, struct ext2fs **fsp, off_t sblockloc, int flags)
{

	return (ext2_sbget(&devfd, fsp, sblockloc, flags, NULL,
	    ext2_use_pread));
}

/*
 * Allocate a buffer of "size" bytes and read into it from byte offset "loc"
 * on the device whose descriptor is pointed at by "devfd".  Used as the
 * read backend for ext2_sbget() in userland, analogous to libufs's
 * use_pread().
 */
static int
ext2_use_pread(void *devfd, off_t loc, void **bufp, int size)
{
	int fd;

	fd = *(int *)devfd;
	*bufp = malloc(size);
	if (*bufp == NULL)
		return (ENOSPC);
	if (pread(fd, *bufp, size, loc) != size) {
		free(*bufp);
		*bufp = NULL;
		return (EIO);
	}
	return (0);
}

/*
 * Write the superblock in disk->d_sb at byte offset "loc".  Byte-granular:
 * backup copies are not block aligned when the block size exceeds 1K.
 */
int
ext2fs_sbwrite(struct ext2fsd *disk, off_t loc)
{

	ERROR(disk, NULL);
	if (pwrite(disk->d_fd, &disk->d_fs, SBLOCKSIZE, loc) != SBLOCKSIZE) {
		ERROR(disk, "superblock write failed");
		return (-1);
	}
	return (0);
}
