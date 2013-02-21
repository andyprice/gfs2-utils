#!/bin/sh

# This script runs gfs2 utils with various options, checking exit codes against
# expected values. If any test fails to exit with an expected code, the exit code
# of the whole script will be non-zero but the tests will continue to be run. The
# sparse file which is used as the target of the tests can be configured by
# setting the environment variables TEST_TARGET (the filename) and TEST_TARGET_SZ
# (its apparent size in gigabytes). Defaults to "test_sparse" and 10GB.

MKFS="${TOPBUILDDIR}/gfs2/mkfs/mkfs.gfs2 -qO"
FSCK="${TOPBUILDDIR}/gfs2/fsck/fsck.gfs2 -qn"

# Name of the sparse file we'll use for testing
TEST_TARGET=${TEST_TARGET:-testvol}
# Log of test output
TEST_LOG=${TEST_LOG:-tests.log}
truncate -cs0 "${TEST_LOG}"
# Size, in GB, of the sparse file we'll create to run the tests
TEST_TARGET_SZ=${TEST_TARGET_SZ:-10}
[ $TEST_TARGET_SZ -gt 0 ] || { echo "Target size (in GB) must be greater than 0" >&2; exit 1; }
# Overall success (so we can keep going if one test fails)
TEST_RET=0

fn_test()
{
	local expected="$1"
	local cmd="$2"
	echo -n "** Test '$cmd'" | tee -a "${TEST_LOG}"
	$cmd &>> "${TEST_LOG}";
	local ret=$?
	echo -n " (exp: $expected got: $ret) " | tee -a "${TEST_LOG}"
	if [ "$ret" != "$expected" ];
	then
		echo "FAIL" | tee -a "${TEST_LOG}"
		TEST_RET=1
		TEST_GRP_RET=1
	else
		echo "PASS" | tee -a "${TEST_LOG}"
	fi
}

fn_rm_target()
{
	fn_test 0 "rm -f $TEST_TARGET"
}

fn_recreate_target()
{
	fn_rm_target
	fn_test 0 "truncate -s ${TEST_TARGET_SZ}G ${TEST_TARGET}"
}


# Tests start here
fn_recreate_target
fn_test 0 "$MKFS -p lock_nolock $TEST_TARGET"
fn_test 0 "$MKFS -p lock_dlm -t foo:bar $TEST_TARGET"
fn_test 255 "$MKFS -p badprotocol $TEST_TARGET"
fn_test 0 "$FSCK $TEST_TARGET"
fn_test 255 "$MKFS -p lock_nolock -r 31 $TEST_TARGET"
fn_test 255 "$MKFS -p lock_nolock -r 2049 $TEST_TARGET"
fn_test 0 "$MKFS -p lock_nolock -r 32 $TEST_TARGET"
fn_test 0 "$FSCK $TEST_TARGET"
fn_test 0 "$MKFS -p lock_nolock -r 2048 $TEST_TARGET"
fn_test 0 "$FSCK $TEST_TARGET"

# Tests end here

# Only remove the test file on success
[ "$TEST_RET" = "0" ] && fn_test 0 "rm -f $TEST_TARGET"
echo "Tool test output written to ${TEST_LOG}"
exit $TEST_RET
