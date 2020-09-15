#!/bin/sh

dev=$1
rgcount=$(gfs2_edit -p rgcount $dev | cut -f1 -d' ')
prevaddr=$(gfs2_edit -p rg 0 $dev | grep ^RG | awk '{print $5}')
prevskip=0
for i in `seq 0 $(($rgcount))`; do
	if [ $i -eq $rgcount ]; then
		if [ $prevskip -ne 0 ]; then
			echo "Last rg_skip is not 0 ($prevskip)" >&2
			exit 1
		fi
		exit 0
	fi
	addr=$(gfs2_edit -p rg $i $dev | grep ^RG | awk '{print $5}')
	expected=$(($addr - $prevaddr))

	if test $prevskip != $expected; then
		echo "Bad rg_skip in rg $(($i - 1)): $prevskip (expected: $expected)" >&2
		exit 1
	fi

	prevskip=$(gfs2_edit -p rg $i $dev | grep rg_skip | awk '{print $2}')
	prevaddr=$addr
done

