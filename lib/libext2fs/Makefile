PACKAGE =	ext2fs
LIB_PACKAGE =

LIB =		ext2fs
SHLIBDIR ?=	/lib
SHLIB_MAJOR =	1

SRCS =		type.c	\
		sblock.c \
		group.c \
		block.c \
		inode.c

# Pull in the dual-mode superblock reader and metadata checksum code
# shared with the kernel ext2fs driver, mirroring the way libufs
# reuses sys/ufs/ffs/ffs_subr.c and sys/libkern/gsb_crc32.c.
.PATH:	${SRCTOP}/sys/fs/ext2fs ${SRCTOP}/sys/libkern
SRCS +=	ext2_sb.c \
	ext2_csum.c \
	gsb_crc32.c \
	crc16.c

INCS =		libext2fs.h
MAN =

CFLAGS +=	-D_LIBEXT2FS
.if defined(LIBEXT2FS_DEBUG)
CFLAGS +=	-D_LIBEXT2FS_DEBUGGING
.endif
CFLAGS +=	-I${.CURDIR}

# TODO: remove this once the library is done:
WARNS ?= 0

.include <bsd.lib.mk>
