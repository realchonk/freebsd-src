#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 The FreeBSD Foundation
#
# This software was developed by Benjamin Stürz <benni@stuerz.xyz>
# under sponsorship from the FreeBSD Foundation.
#

# newfs_ext2fs(8) tests.  Everything works on regular file images, so no
# root privilege or md(4) is required.  dumpfs_ext2fs(8) and fsck_ext2fs(8)
# verify the result; dumpe2fs(8) is an optional oracle.

NEWFS_EXT2FS="${NEWFS_EXT2FS:-/sbin/newfs_ext2fs}"
DUMPFS_EXT2FS="${DUMPFS_EXT2FS:-/sbin/dumpfs_ext2fs}"
FSCK_EXT2FS="${FSCK_EXT2FS:-/sbin/fsck_ext2fs}"

make_image()
{
	atf_check -s exit:0 truncate -s 32m "$1"
}

dump_super()
{
	atf_check -s exit:0 -o save:dump.out "$DUMPFS_EXT2FS" -s "$1"
}

# First token after the given key in a dumpfs_ext2fs line.
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

# Field value from dumpe2fs -h output.
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

assert_clean()
{
	atf_check -s exit:0 -o save:fsck.log "$FSCK_EXT2FS" -n "$1"
	atf_check -s exit:1 grep -Eq \
	    'WRONG|BAD|UNREF|MISSING|SALVAGE|DUP|FREE SPACE' fsck.log
}

atf_test_case default
default_head() { atf_set descr "newfs_ext2fs creates a clean ext2 file system"; }
default_body()
{
	img=./default.img
	make_image "$img"

	atf_check -s exit:0 -o save:newfs.log "$NEWFS_EXT2FS" "$img"
	atf_check -s exit:0 grep -Eq 'block size 4096' newfs.log

	dump_super "$img"
	atf_check test "$(field "block size" dump.out)" = 4096
	atf_check test "$(field "total blocks" dump.out)" = 8192
	atf_check test "$(field "inode size" dump.out)" = 128
	assert_clean "$img"
}

atf_test_case nowrite
nowrite_head() { atf_set descr "-N computes parameters without writing"; }
nowrite_body()
{
	img=./nowrite.img
	make_image "$img"

	before=$(md5 -q "$img")
	atf_check -s exit:0 -o save:newfs.log "$NEWFS_EXT2FS" -N "$img"
	atf_check -o inline:"$before"$'\n' md5 -q "$img"
	atf_check -s exit:0 grep -Eq 'block size 4096' newfs.log
}

atf_test_case label
label_head() { atf_set descr "-L sets the volume label"; }
label_body()
{
	img=./label.img
	make_image "$img"

	atf_check -s exit:0 -o save:newfs.log \
	    "$NEWFS_EXT2FS" -L TESTLABEL "$img"
	dump_super "$img"
	atf_check test "$(field "volume name" dump.out)" = TESTLABEL
	assert_clean "$img"
}

atf_test_case badlabel
badlabel_head() { atf_set descr "-L rejects an over-long label"; }
badlabel_body()
{
	img=./badlabel.img
	make_image "$img"

	atf_check -s exit:17 -e match:"volume label too long" \
	    "$NEWFS_EXT2FS" -L 1234567890123456 "$img"
}

atf_test_case bsize
bsize_head() { atf_set descr "-b selects the block size"; }
bsize_body()
{
	img=./bsize.img
	make_image "$img"

	atf_check -s exit:0 -o save:newfs.log "$NEWFS_EXT2FS" -b 1024 "$img"
	dump_super "$img"
	atf_check test "$(field "block size" dump.out)" = 1024
	atf_check test "$(field "total blocks" dump.out)" = 32768
	atf_check test "$(field "first data block" dump.out)" = 1
	assert_clean "$img"
}

atf_test_case density
density_head()
{
	atf_set descr "-i and -m set inode density and reserved blocks"
}
density_body()
{
	img=./density.img
	make_image "$img"

	atf_check -s exit:0 -o save:newfs.log \
	    "$NEWFS_EXT2FS" -i 8192 -m 3 "$img"
	dump_super "$img"

	total=$(field "total blocks" dump.out)
	reserved=$(field "reserved blocks" dump.out)
	atf_check test "$reserved" -eq $((total * 3 / 100))
	atf_check test "$(field "total inodes" dump.out)" = 16384
	assert_clean "$img"
}

atf_test_case size
size_head() { atf_set descr "-s limits the file system size"; }
size_body()
{
	img=./size.img
	make_image "$img"

	atf_check -s exit:0 -o save:newfs.log \
	    "$NEWFS_EXT2FS" -s 16m "$img"
	atf_check -o inline:"33554432\n" stat -f %z "$img"

	dump_super "$img"
	atf_check test "$(field "total blocks" dump.out)" = 4096
	assert_clean "$img"
}

atf_test_case oracle
oracle_head()
{
	atf_set descr "newfs_ext2fs parameters agree with dumpe2fs"
}
oracle_body()
{
	img=./oracle.img
	atf_require_prog dumpe2fs
	make_image "$img"

	atf_check -s exit:0 -o ignore \
	    "$NEWFS_EXT2FS" -b 1024 -i 8192 -m 3 -L ORACLE "$img"
	atf_check -s exit:0 -o save:dump.out "$DUMPFS_EXT2FS" -s "$img"
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h "$img"

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
	atf_add_test_case default
	atf_add_test_case nowrite
	atf_add_test_case label
	atf_add_test_case badlabel
	atf_add_test_case bsize
	atf_add_test_case density
	atf_add_test_case size
	atf_add_test_case oracle
}
