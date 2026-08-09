PACKAGE =	ext2fs
LIB_PACKAGE =

LIB =		ext2fs
SHLIBDIR ?=	/lib
SHLIB_MAJOR =	1

SRCS =		type.c	\
		sblock.c

# Pull in the dual-mode superblock reader shared with the kernel ext2fs
# driver, mirroring the way libufs reuses sys/ufs/ffs/ffs_subr.c.
.PATH:	${SRCTOP}/sys/fs/ext2fs
SRCS +=	ext2_sb.c

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
