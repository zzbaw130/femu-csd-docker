#!/bin/sh
# 调用方式为 在宿主机运行 sh tests/guest/queries/selectivity/generate.sh <目标选择率> [SF]。
# 目标选择率为 0 到 1，SF 默认 8，也可通过环境变量 SF 指定。
# 从 Guest 的 tpch_s<SF> 校准并写出 sf<SF>_sel<目标> 下的 22 条 SQL。
# Run calibration in the FEMU Guest and write concrete SQL on the host.
set -eu

target=${1:-}
case "$target" in *[!0-9.]*) target= ;; esac
sf=${2:-${SF:-8}}
case "$sf" in *[!0-9.]*) sf= ;; esac
if [ "$#" -gt 2 ] || ! printf '%s\n' "$target" | grep -Eq '^(0([.][0-9]+)?|1([.]0+)?)$' ||
   ! printf '%s\n' "$sf" | grep -Eq '^[0-9]+([.][0-9]+)?$'; then
    echo "用法：$0 <0 到 1 的目标选择率> [SF]" >&2
    exit 2
fi

calibration=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
queries=$(dirname -- "$calibration")
root=$(CDPATH= cd -- "$calibration/../../../.." && pwd)
output=$queries/sf${sf}_sel$target
if [ -e "$output" ]; then
    echo "输出目录已存在：$output" >&2
    exit 1
fi

scratch=$(mktemp -d "$queries/.sf${sf}_sel$target.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
trap 'exit 1' HUP INT TERM
mkdir "$scratch/sql"
tab=$(printf '\t')

for query in $(seq 22); do
    printf '正在校准 Q%s\n' "$query" >&2
    {
        printf 'SET pg_sina_csd.exe_path = %s;\n' "'host-only'"
        cat "$queries/../tpch-parameters.psql"
        printf 'WITH calibrated AS (\n'
        sed '$s/;$//' "$calibration/$query.sql"
        cat <<'SQL'
)
SELECT '@rate', format('%s: %s/%s (selectivity=%s)',
                       filter_scope, matched_rows, input_rows, actual_selectivity)
FROM calibrated
UNION
SELECT name, quote_literal(coalesce(value, CASE name WHEN 'q8_nation' THEN :'q8_nation' END))
FROM calibrated
CROSS JOIN LATERAL jsonb_each_text(parameters) AS p(name, value)
ORDER BY 1, 2;
SQL
    } > "$scratch/calibration.psql"
    docker compose -f "$root/docker-compose.yml" exec -T femu-csd ./run-guest-ssh.sh \
        "/home/femu/opt/postgres/release/bin/psql -XqAt -F '$tab' -v ON_ERROR_STOP=1 -v target=$target -d tpch_s$sf -f -" \
        < "$scratch/calibration.psql" > "$scratch/values.tsv"

    awk -F '\t' -v target="$target" -v sf="$sf" '
        BEGIN { print "-- SF" sf " target selectivity: " target }
        FILENAME == ARGV[1] {
            if ($1 == "@rate") print "-- " $2
            else if ($2 == "" || ($1 in values && values[$1] != $2)) {
                print "Missing or conflicting value for " $1 > "/dev/stderr"
                exit 1
            } else values[$1] = $2
            next
        }
        /^\\if :\{\?q8_nation\}$/ { skip_default = 1; next }
        skip_default && /^\\endif$/ { skip_default = 0; next }
        skip_default { next }
        {
            rest = $0
            rendered = ""
            while (match(rest, /:\047q[0-9]+_[a-z0-9_]+\047/)) {
                key = substr(rest, RSTART + 2, RLENGTH - 3)
                if (!(key in values)) {
                    print "Missing value for " key > "/dev/stderr"
                    exit 1
                }
                rendered = rendered substr(rest, 1, RSTART - 1) values[key]
                rest = substr(rest, RSTART + RLENGTH)
            }
            print rendered rest
        }
    ' "$scratch/values.tsv" "$queries/template/$query.sql" > "$scratch/sql/$query.sql"
done

mv -T -- "$scratch/sql" "$output"
printf '%s\n' "$output"
