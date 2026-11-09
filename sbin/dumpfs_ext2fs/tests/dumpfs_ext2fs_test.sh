#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 The FreeBSD Foundation
#
# This software was developed by Benjamin Stürz <benni@stuerz.xyz>
# under sponsorship from the FreeBSD Foundation.
#

# dumpfs_ext2fs(8) tests.  The fixture was made once with mke2fs, with a
# fixed UUID and label; the golden files were produced from that exact
# image.  dumpe2fs(8) is used as an optional live oracle.

DUMPFS_EXT2FS="${DUMPFS_EXT2FS:-/sbin/dumpfs_ext2fs}"

decompress()
{
	bzcat "$(atf_get_srcdir)/ext2.img.bz2" > ./ext2.img
}

field()
{
	awk -v key="$1" '
	    {
		line = $0
		gsub(/[[:space:]]+/, " ", line)
		n = split(line, f, / +/)
		m = split(key, k, / +/)
		for (i = 1; i + m - 1 <= n; i++) {
		    ok = 1
		    for (j = 1; j <= m; j++)
			if (f[i + j - 1] != k[j]) {
			    ok = 0
			    break
			}
		    if (ok) {
			print f[i + m]
			exit
		    }
		}
	    }' "$2"
}

dumpe_field()
{
	awk -F: -v key="$1" '
	    $1 == key {
		value = $2
		sub(/^[[:space:]]+/, "", value)
		sub(/[[:space:]]+$/, "", value)
		print value
		exit
	    }' "$2"
}

atf_test_case superblock
superblock_head() { atf_set descr "-s matches the committed fixture output"; }
superblock_body()
{
	decompress
	atf_check -s exit:0 -o file:"$(atf_get_srcdir)/ext2_super.out" \
	    "$DUMPFS_EXT2FS" -s ./ext2.img
}

atf_test_case full
full_head() { atf_set descr "the full dump matches the committed fixture"; }
full_body()
{
	decompress
	atf_check -s exit:0 -o file:"$(atf_get_srcdir)/ext2_full.out" \
	    "$DUMPFS_EXT2FS" ./ext2.img
}

atf_test_case free
free_head() { atf_set descr "-f prints collapsed free block ranges"; }
free_body()
{
	decompress
	atf_check -s exit:0 -o file:"$(atf_get_srcdir)/ext2_free.out" \
	    "$DUMPFS_EXT2FS" -f ./ext2.img
}

atf_test_case free_all
free_all_head() { atf_set descr "-ff prints every free block separately"; }
free_all_body()
{
	decompress
	atf_check -s exit:0 -o save:free-ff.out \
	    "$DUMPFS_EXT2FS" -ff ./ext2.img
	atf_check test "$(awk 'END { print NR }' free-ff.out)" -eq 15211
	atf_check -o inline:"594"$'\n' head -1 free-ff.out
	atf_check -o inline:"16383"$'\n' tail -1 free-ff.out
}

atf_test_case oracle
oracle_head()
{
	atf_set descr "dumpfs_ext2fs fields agree with dumpe2fs"
}
oracle_body()
{
	atf_require_prog dumpe2fs
	decompress

	atf_check -s exit:0 -o save:dump.out "$DUMPFS_EXT2FS" -s ./ext2.img
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h ./ext2.img

	atf_check test "$(field "block size" dump.out)" = \
	    "$(dumpe_field "Block size" dumpe.out)"
	atf_check test "$(field "total blocks" dump.out)" = \
	    "$(dumpe_field "Block count" dumpe.out)"
	atf_check test "$(field "total inodes" dump.out)" = \
	    "$(dumpe_field "Inode count" dumpe.out)"
	atf_check test "$(field "free blocks" dump.out)" = \
	    "$(dumpe_field "Free blocks" dumpe.out)"
	atf_check test "$(field "free inodes" dump.out)" = \
	    "$(dumpe_field "Free inodes" dumpe.out)"
	atf_check test "$(field "reserved blocks" dump.out)" = \
	    "$(dumpe_field "Reserved block count" dumpe.out)"
	atf_check test "$(field "volume name" dump.out)" = \
	    "$(dumpe_field "Filesystem volume name" dumpe.out)"
	atf_check test "$(field "UUID" dump.out)" = \
	    "$(dumpe_field "Filesystem UUID" dumpe.out)"
}

atf_init_test_cases()
{
	atf_add_test_case superblock
	atf_add_test_case full
	atf_add_test_case free
	atf_add_test_case free_all
	atf_add_test_case oracle
}
