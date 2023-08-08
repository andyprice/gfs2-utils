#!/bin/bash

# Pipe a list of metadata files to this script to run fsck.gfs2 tests against them
# Usage:
#   fsck.gfs2-tester.sh <path> <truncate_size>
#
#            path: Path of the writeble device or file to test on (contents will be destroyed)
#   truncate_size: Size of sparse file to create at path (if not a block device)
#
# Input format is:
#   <clean|dirty> <path>
#
# Relative paths will be relative to the current working directory
#
# To test a different fsck.gfs2 (or gfs2_edit) adjust your PATH accordingly

FSCK=fsck.gfs2
GFS2EDIT=gfs2_edit

device="$1"
if ! touch "$device"
then
	echo "Invalid test device" >&2
	exit 1
fi

truncate_size="$2"
if [ "x$truncate_size" = "x" ]
then
	truncate_size=0
fi

timestamp=$(date +%Y-%m-%d_%H:%M:%S)
logdir="fsck.gfs2.test.results.${timestamp}"
resultsfile="${logdir}/results.log"
failsfile="${logdir}/fsck.gfs2.fails.in"

function log()
{
	printf "$1" >> "$logfile"
}

function log_start()
{
	echo -n "*** $(basename "$2") ($1) ... "
	echo -n "*** $2 ($1) ... " >> "$resultsfile"
}

function log_result()
{
	echo "$1" | tee -a "$resultsfile"
}

function log_failure()
{
	echo $1 "$2" >> "$failsfile"
	log_result Fail
}

function rmdev()
{
	[ "$truncate_size" != "0" ] && rm -vf "$device"
	return 0
}

function _truncate()
{
	[ "$truncate_size" != "0" ] && truncate -s "$truncate_size" "$device"
	return 0
}

function test_restore()
{
	local mdata=$1

	(
	rmdev &&
	_truncate &&
	$GFS2EDIT restoremeta $mdata $device
	) &>> "$logfile"
}

function test_clean()
{
	local mdata="$1"

	log_start clean "$mdata"
	test_restore "$mdata"
	rc=$?
	if [ $rc -ne 0 ]; then
		log "restoremeta finished with result $rc"
		log_failure clean "$mdata"
		return $rc
	fi
	echo "Running: $FSCK -n $device" >> "$logfile"
	$FSCK -n "$device" &>> "$logfile"
	rc=$?
	if [ $rc -ne 0 ]; then
		log "$FSCK -n $device finished with result $rc"
		log_failure clean "$mdata"
		return $rc
	fi
	log_result "Pass"
}

function test_dirty()
{
	local mdata=$1

	log_start dirty "$mdata"
	test_restore "$mdata"
	rc=$?
	if [ $rc -ne 0 ]; then
		log "restoremeta finished with result $rc"
		log_failure dirty "$mdata"
		return $rc
	fi
	echo "Running: $FSCK -y $device" >> "$logfile"
	$FSCK -y "$device" &>> "$logfile"
	rc=$?
	if [ $rc -ne 1 ]; then
		log "$mdata: Return code on dirty fsck was $rc, should be 1"
		log_failure dirty "$mdata"
		return $rc
	fi
	echo "Running: $FSCK -n $device" >> "$logfile"
	$FSCK -n "$device" &>> "$logfile"
	rc=$?
	if [ $rc -ne 0 ]; then
		log "$mdata: fsck.gfs2 after repair returned $rc, expected 0"
		log_failure dirty "$mdata"
		return $rc
	fi
	log_result "Pass"
}

tests_run=0
tests_failed=0

mkdir -p "${logdir}"
rm -f "${logdir}/"*.out
while read exp file
do
	logfile="${logdir}/$(basename $file).out"
	echo "Using PATH: $PATH" > "$logfile"
	if [ "$exp" = "dirty" ]
	then
		((tests_run++))
		test_dirty "$file"
		rc=$?
	elif [ "$exp" = "clean" ]
	then
		((tests_run++))
		test_clean "$file"
		rc=$?
	else
		echo "Error in input file: $exp" >&2
		rm "$logfile"
		exit 1
	fi
	if [ $rc -eq 0 ]
	then
		rm "$logfile"
	else
		((tests_failed++))
	fi
done

echo "Tests run: $tests_run"
echo "Tests failed: $tests_failed"

if [ $tests_failed -ne 0 ]
then
	echo "See $logdir/*.out for failed test logs"
	echo "Use $failsfile to re-run failed tests"
	exit 1
fi
exit 0
