PACKAGE =	ext2fs
LIB_PACKAGE =

LIB =		ext2fs
SHLIBDIR ?=	/lib
SHLIB_MAJOR =	1

SRCS =		type.c	\
		sblock.c
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
