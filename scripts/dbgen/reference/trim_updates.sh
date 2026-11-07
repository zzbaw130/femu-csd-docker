#!/bin/bash
# Run bash trim_updates.sh <set> <linecount>. DSS_PATH defaults to the current directory.
# set selects the update file suffix. linecount is the maximum number of lines retained in each file.
if [ $# -ne 2 ]
then
	echo "USAGE: `basename $0` <set> <linecount>"
	echo "	trim all files in the named update set, retaining only the first"
	echo "	<linecount> lines"
	exit
fi
if [ -z "$DSS_PATH" ]
then
	DSS_PATH=.
	export DSS_PATH
fi
tmp=`mktemp`
for f in $DSS_PATH/*u$1.*
do
	head -$2 $f > $tmp
	mv $tmp $f
done
