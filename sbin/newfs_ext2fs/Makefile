.include <src.opts.mk>

PACKAGE =	ext2fs
PROG =		newfs_ext2fs
SRCS =		newfs_ext2fs.c mkfs_ext2fs.c
LIBADD =	ext2fs util
MAN =		newfs_ext2fs.8

WARNS ?=	3

HAS_TESTS=	yes
SUBDIR.${MK_TESTS}=	tests

.include <bsd.prog.mk>
