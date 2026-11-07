#!/bin/bash
# Run bash check_dirs.sh [directories] from the DBGEN test directory.
# directories is a space-delimited list of result directories, defaulting to the 1* and 300* patterns.

er=0

if [ -z "$1" ]; then
  DIRS='1* 300*'
else
  DIRS=$1
fi

for d in $DIRS;do
  echo Checking dir $d
  cd $d
  for f in results.*;do
    c=`sed 's/^[0-9]* *//' <$f |uniq |wc -l`
    if [ $c -ne 1 ]; then
      echo BAD RESULT $d/$f
      er=1;
    fi
  done
  cd ..
done
if [ $er -eq 0 ]; then
   echo ALL TESTS PASSED
fi

