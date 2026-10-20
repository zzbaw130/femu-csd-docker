#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case ${1:-unit} in
    unit)
        cmake -S "$ROOT/code/csd-programs" -B /tmp/csd-programs-check -DCMAKE_BUILD_TYPE=RelWithDebInfo
        cmake --build /tmp/csd-programs-check -j2
        ctest --test-dir /tmp/csd-programs-check --output-on-failure
        cmake -S "$ROOT/code/apps/snia-cs-api" -B /tmp/snia-cs-api-check \
            -DCSD_TEST_PROGRAM_DIR=/tmp/csd-programs-check -DCMAKE_BUILD_TYPE=RelWithDebInfo
        cmake --build /tmp/snia-cs-api-check -j2
        ctest --test-dir /tmp/snia-cs-api-check --output-on-failure
        ;;
    build)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd sh -c \
            'cmake -S /workspace/code/csd-programs -B /workspace/code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /workspace/code/csd-programs/build -j2'
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'cmake -S /home/femu/apps/snia-cs-api -B /tmp/snia-cs-api-guest -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /tmp/snia-cs-api-guest -j2'
        # Built with the container's Guest-compatible toolchain. Preserve the
        # executable's relative RPATH to the same workmem library used by CSFs.
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd env SSH_PORT="${SSH_PORT:-8080}" sh -c \
            'tar -C /workspace/code/csd-programs/build -cf - examples/read-merge-sort-avg examples/read-merge-path-sort-avg libcs-workmem.so | ./run-guest-ssh.sh "mkdir -p /tmp/csd-sort-bench; tar -xf - -C /tmp/csd-sort-bench"'
        ;;
    smoke|example)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo /tmp/snia-cs-api-guest/cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin'
        ;;
    bench)
        bash "$0" build
        RESULT_DIR=$(mktemp -d /tmp/snia-merge-compare.XXXXXX)
        printf 'algorithm\tmode\tround\tstage1_s\tstage2_s\ttotal_s\n' > "$RESULT_DIR/timings.tsv"
        printf 'algorithm\tmode\tround\toperation\tcalls\twall_s\tcpu_s\trun_sort_s\tread_bytes\n' > "$RESULT_DIR/profiles.tsv"
        reference=''
        declare -A reference_workload
        for round in 1 2 3; do
            if (( round % 2 )); then methods=(merge-sort:CPU merge-path:CPU merge-sort:CSD merge-path:CSD);
            else methods=(merge-path:CSD merge-sort:CSD merge-path:CPU merge-sort:CPU); fi
            for method in "${methods[@]}"; do
                algorithm=${method%:*}; mode=${method#*:}
                echo "Round $round: $algorithm $mode"
                if [[ $mode == CPU ]]; then
                    if [[ $algorithm == merge-sort ]]; then binary=read-merge-sort-avg; else binary=read-merge-path-sort-avg; fi
                    guest_command="sudo /tmp/csd-sort-bench/examples/$binary /data/cs-batch-input.bin"
                else
                    guest_command='sudo env SNIA_FEMU_PROFILE=1 stdbuf -oL /tmp/snia-cs-api-guest/cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin --existing'
                    if [[ $algorithm == merge-path ]]; then guest_command+=' --merge-path'; fi
                fi
                logfile="$RESULT_DIR/$algorithm-$mode-$round.log"
                docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
                    env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh "$guest_command" 2>&1 | tee "$logfile"
                result=$(sed -n 's/^AVERAGE //p' "$logfile")
                if [[ -z $reference ]]; then reference=$result; fi
                [[ -n $result && $result == "$reference" && $result == count=67108864\ * ]] || { echo 'FAIL: incomplete or inconsistent result' >&2; exit 1; }
                workload=$(awk '/^WORKLOAD/ { print $3, $4, $5, $6 }' "$logfile")
                if [[ -z ${reference_workload[$algorithm]:-} ]]; then reference_workload[$algorithm]=$workload; fi
                [[ -n $workload && $workload == "${reference_workload[$algorithm]}" ]] || { echo 'FAIL: CPU/CSD workload mismatch' >&2; exit 1; }
                awk '/^WORKLOAD/ { split($7,p,"="); split($8,c,"="); if (p[2]>c[2] || c[2]!=67108864) exit 1 }' "$logfile"
                awk -v a="$algorithm" -v r="$round" '$1=="TIMING" { printf "%s\t%s\t%d\t%s\t%s\t%s\n",a,$2,r,$3,$4,$5; found++ }
                    END { if(found!=1) exit 1 }' "$logfile" >> "$RESULT_DIR/timings.tsv"
                awk -v a="$algorithm" -v r="$round" 'BEGIN { OFS="\t" }
                    $1=="PROFILE" {
                        for(i=4;i<=NF;++i) { split($i,p,"="); v[p[1]]=p[2] }
                        print a,$2,r,$3,v["calls"],v["wall_s"],v["cpu_s"],v["run_sort_s"],v["read_bytes"]; delete v
                    }' "$logfile" >> "$RESULT_DIR/profiles.tsv"
            done
        done
        awk 'NR>1 { key=$1" "$2; total[key]+=$6; n[key]++ }
            END { for(key in n) printf "%s mean=%.6fs\n",key,total[key]/n[key] }' "$RESULT_DIR/timings.tsv"
        echo "Results: $RESULT_DIR"
        ;;
    *) echo '用法：snia-cs-api-test.sh [unit|build|smoke|example|bench]；example 写入256MiB数据文件' >&2; exit 2 ;;
esac
