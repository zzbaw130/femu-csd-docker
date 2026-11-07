#!/bin/bash
# Run bash column_split.sh <file_prefix> with pipe-delimited rows on stdin.
# The prefix names the output files <file_prefix>1, <file_prefix>2, and so on.
TABLE=$1
IFS="|"

while read line
do
	set $line
	count=1
	while [ $# -gt 0 ]
	do
		echo $1 >> ${TABLE}$count
		count=`expr $count + 1`
		shift
	done
done
