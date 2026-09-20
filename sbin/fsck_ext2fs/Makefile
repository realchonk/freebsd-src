PACKAGE =	ext2fs
PROG =		fsck_ext2fs
SRCS =		main.c setup.c fsutil.c inode.c pass1.c pass2.c pass3.c pass4.c pass5.c globals.c
LIBADD =	ext2fs util
MAN =		fsck_ext2fs.8
WARNS ?=	3
CFLAGS +=	-I${.CURDIR}

.include <bsd.prog.mk>
