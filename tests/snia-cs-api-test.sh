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
            'tar -C /workspace/code/csd-programs/build -cf - examples/read-sort-avg libcs-workmem.so | ./run-guest-ssh.sh "mkdir -p /tmp/csd-sort-bench; tar -xf - -C /tmp/csd-sort-bench"'
        ;;
    smoke|example)
        docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
            env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh \
            'sudo /tmp/snia-cs-api-guest/cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin'
        ;;
    bench)
        bash "$0" build
        RESULT_DIR=$(mktemp -d /tmp/snia-sort-avg.XXXXXX)
        printf 'mode\tround\tstage1_s\tstage2_s\ttotal_s\n' > "$RESULT_DIR/timings.tsv"
        printf 'mode\tround\toperation\tcalls\twall_s\tcpu_s\trun_sort_s\n' > "$RESULT_DIR/profiles.tsv"
        reference=''
        reference_workload=''
        for round in 1 2 3; do
            if (( round % 2 )); then methods=(CPU CSD); else methods=(CSD CPU); fi
            for method in "${methods[@]}"; do
                echo "Round $round: $method"
                if [[ $method == CPU ]]; then
                    guest_command='sudo /tmp/csd-sort-bench/examples/read-sort-avg /data/cs-batch-input.bin'
                else
                    guest_command='sudo env SNIA_FEMU_PROFILE=1 stdbuf -oL /tmp/snia-cs-api-guest/cs-batch-sort-avg /dev/nvme0n1 /data/cs-batch-input.bin --existing'
                fi
                logfile="$RESULT_DIR/$method-$round.log"
                docker compose -f "$ROOT/docker-compose.yml" exec -T femu-csd \
                    env SSH_PORT="${SSH_PORT:-8080}" ./run-guest-ssh.sh "$guest_command" 2>&1 | tee "$logfile"
                result=$(sed -n 's/^AVERAGE //p' "$logfile")
                if [[ -z $reference ]]; then reference=$result; fi
                if [[ -z $result || $result != "$reference" ]]; then echo 'FAIL: average/count/sum mismatch' >&2; exit 1; fi
                [[ $result == count=67108864\ * ]] || { echo 'FAIL: incomplete sorted stream' >&2; exit 1; }
                workload=$(awk '/^WORKLOAD/ { print $3, $4, $5, $6 }' "$logfile")
                if [[ -z ${reference_workload:-} ]]; then reference_workload=$workload; fi
                if [[ -z $workload || $workload != "$reference_workload" ]]; then echo 'FAIL: spill workload mismatch' >&2; exit 1; fi
                awk '/^WORKLOAD/ { split($7, peak, "="); split($8, cap, "=");
                    if (peak[2] > cap[2] || cap[2] != 67108864) exit 1 }' "$logfile"
                awk -v round="$round" '$1 == "TIMING" { printf "%s\t%d\t%s\t%s\t%s\n", $2, round, $3, $4, $5; found++ }
                    END { if (found != 1) exit 1 }' "$logfile" >> "$RESULT_DIR/timings.tsv"
                awk -v round="$round" 'BEGIN { OFS="\t" }
                    $1 == "PROFILE" {
                        for (i=4; i<=NF; ++i) { split($i, pair, "="); value[pair[1]]=pair[2] }
                        print $2, round, $3, value["calls"], value["wall_s"], value["cpu_s"], value["run_sort_s"];
                        delete value
                    }' "$logfile" >> "$RESULT_DIR/profiles.tsv"
            done
        done
        awk 'NR > 1 { total[$1] += $5; n[$1]++ }
            END { cpu = total["CPU"] / n["CPU"]; csd = total["CSD"] / n["CSD"];
                printf "Mean CPU=%.3fs, CSD=%.3fs, CPU/CSD=%.3fx\n", cpu, csd, cpu/csd }' "$RESULT_DIR/timings.tsv"
        echo "Results: $RESULT_DIR"
        ;;
    *) echo '用法：snia-cs-api-test.sh [unit|build|smoke|example|bench]；example 写入256MiB数据文件' >&2; exit 2 ;;
esac
