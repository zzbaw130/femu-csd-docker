#!/bin/sh
# Guest: sh M1-state-growth.sh /absolute/output [--from-logs]
# The extraction-only option can also run in the mounted host workspace.
set -eu
out=${1:?supply an absolute output directory}
source=${2:-measure}
sizes='262144 1048576 4194304 8388608'
mkdir -p "$out/logs"
if [ "$source" = measure ]; then
    export PATH=/home/femu/opt/postgres/release/bin:$PATH
    export PGDATABASE=tpch_s8
    export PGOPTIONS='-c max_parallel_workers_per_gather=0 -c work_mem=1536MB'
    for n in $sizes; do
        # The anti join keeps the probe size fixed and builds the right-hand table.
        psql -X -qAt -v ON_ERROR_STOP=1 -c "SET enable_mergejoin=off; SET enable_nestloop=off;
          EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF)
          SELECT count(*) FROM generate_series(1,1024) AS a(k)
          WHERE NOT EXISTS (SELECT 1 FROM generate_series(1,$n) AS b(k) WHERE a.k=b.k)" > "$out/logs/hash-$n.txt"
        psql -X -qAt -v ON_ERROR_STOP=1 -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF)
          SELECT k, k::bigint AS a, k::bigint+1 AS b, k::bigint+2 AS c
          FROM generate_series(1,$n) AS x(k) ORDER BY k DESC" > "$out/logs/sort-$n.txt"
        psql -X -qAt -v ON_ERROR_STOP=1 -c "SELECT count(*), sum(s1), sum(s2), sum(s3) FROM (
          SELECT k::bigint, sum(k) AS s1, sum(k+1) AS s2, sum(k+2) AS s3
          FROM generate_series(1,$n) AS x(k) GROUP BY k::bigint
        ) AS groups" > "$out/logs/groups-$n.txt"
    done
else
    [ "$source" = --from-logs ]
fi

# Validate measured state and group counts before publishing the CSV.
tmp=$(mktemp "$out/.M1-state-growth.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM
printf '%s\n' 'operator,input_rows,state_bytes,metric' > "$tmp"
for n in $sizes; do
    awk -v n="$n" '
      /Buckets:.*Batches: 1.*Memory Usage:/ {kb=$NF+0; check=1; next}
      check && /actual rows=/ {split($0,a,"actual rows="); if(a[2]+0==n) ok=1; check=0}
      END {if (!ok || !kb) exit 1; printf "Hash join,%d,%.0f,PG node peak\n",n,kb*1024}
    ' "$out/logs/hash-$n.txt" >> "$tmp"
    awk -v n="$n" '
      /Sort Method: quicksort  Memory:/ {kb=$NF+0}
      END {if (!kb) exit 1; printf "Sort,%d,%.0f,PG node peak\n",n,kb*1024}
    ' "$out/logs/sort-$n.txt" >> "$tmp"
    # One 64-bit key and three 64-bit SUM transition values per group.
    # The reported lower bound excludes hash metadata and alignment.
    awk -F '|' -v n="$n" 'NR==1 && $1==n {printf "Aggregate,%d,%.0f,Payload lower bound\n",n,n*32; ok=1} END{if(!ok) exit 1}' \
        "$out/logs/groups-$n.txt" >> "$tmp"
done
chmod 644 "$tmp"
mv -f "$tmp" "$out/M1-state-growth.csv"
