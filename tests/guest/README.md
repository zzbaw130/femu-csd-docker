# TPC-H 全量测试

在 16 GiB Guest 中使用三个入口，默认数据库为 SF8（`tpch_s8`），默认目标选择率为 0.1。五种 exe_path 的预热和正式测量都读取 `queries/sf8_sel0.1/` 下已生成的 SQL。可用 `SF=4` 等环境变量选择其他已启动规模，用 `-s` 选择已生成的目标选择率。测试前先在宿主机运行下文的生成命令。Guest 挂载路径为 `/home/femu/tests`，`PGDATA` 和 `PGDATABASE` 可单独指定路径和数据库。

参数名按查询编号和用途命名，Q17 和 Q20 文件末尾保留使用同一组参数的原始相关子查询。单独运行模板时，可传入自己的参数值。

```sh
psql -X -v ON_ERROR_STOP=1 -d tpch_s8 \
  -v q6_start_date=1995-01-01 -v q6_discount=0.06 -v q6_quantity_limit=25 \
  -f /home/femu/tests/queries/template/6.sql
```

```sh
export PGDATA=/data/tpch_s8 PGDATABASE=tpch_s8
sh /home/femu/tests/tpch-cold.sh   -n 3 -o /home/femu/results/tpch-sf8
sh /home/femu/tests/tpch-warm.sh   -n 3 -o /home/femu/results/tpch-sf8
sh /home/femu/tests/tpch-repeat.sh -n 5 -o /home/femu/results/tpch-sf8
```

`-n` 指定测试次数，`-s` 指定目标选择率（默认 0.1，写法需与生成目录一致），`-b` 指定 shared_buffers（默认 2GB），`-w` 指定 max_parallel_workers_per_gather（默认 4），`-m` 指定 work_mem（默认 16MB），`-D` 指定 PGDATA，`-d` 指定数据库，`-o` 指定结果目录。默认结果目录名包含选择率，避免不同组覆盖。内存也接受 `2G`、`16M` 写法。全局 parallel_workers/worker_processes 至少为 8 或指定 worker 数，以免限制每个 Gather 的配置。可在选项后指定少量查询编号，例如 `-s 0.1 -n 1 1`。正式全量省略查询编号。

- 冷启动：每次测量前停止PG、sync、清Guest page cache、重启PG。
- 热启动：每条SQL、每种模式先清两层缓存，原生HOST执行该SQL一次，再在同一连接连续测n次。预热不计时。
- 重复启动：每条SQL、每种模式从冷态直接连续执行n次，不做HOST预热；相邻执行停顿1秒给异步观测缓冲，停顿不计查询耗时。

切换SQL或exe_path时均隔离缓存。前三个模式关闭HPCP/DCP，`csd-with-hpcp`和`SQLerate`同时开启。连接初始化及CSF加载在正式计时之外；不在组内运行采样SQL。`tpch-test.sh`只保存公共执行和CSV整理，三个入口复用它，不维护三套重复实现。

可用环境变量 `TPCH_MODES='host-only filter-only csd-with-hpcp SQLerate'` 选择部分模式，CSV列随实际模式生成。每组查询前后保存 `*.cache-before/after`，其内核累计计数差表示这组实际HPCP填充、DMA与丢弃字节；DCP填充页数沿用对应 `*.err` 中的统计。快照在SQL计时之外采集，不启用额外逐页计时。累计填充字节可能包含重复，不能直接当作最终唯一页面驻留量。

每次全量完成产生两份CSV，三个入口合计六份：

| 文件 | 内容 |
| --- | --- |
| `cold.csv`、`warm.csv`、`repeat.csv` | 每行Q1-1这样的编号，五种模式原始毫秒数、各自ExecutorEnd维护时间、正确性及FDM状态 |
| `cold-sqlerate.csv`、`warm-sqlerate.csv`、`repeat-sqlerate.csv` | 每次SQLerate的规划、执行、脚本总时间、ExecutorEnd维护时间、缓存驻留和实际路径 |

每份CSV恰好22×n条记录，不提前求中位数。错误也保留该行的原始时间和失败状态；`checks`应全部为ok，`fdm_clean`应为yes，SQLerate详情status应为ok。Q15总时间包括CREATE/SELECT/DROP，执行时间为主SELECT的执行器计时，因此不能强求规划+执行等于脚本总时间。

`*_ms`和`total_ms`是原始psql时间。按项目既定口径分析查询主体时，减去对应`*_executor_end_ms`；原始值不可覆盖。热态和重复CSV还保存五种模式的`*_cache_residency`：`shared`、`file`、`known`分别为已观测shared_buffers驻留、文件缓存驻留、文件观测覆盖百分比。分母是当前数据库用户表及索引main fork总页数，**不是当前SQL工作集命中率**，两层缓存不可相加。数据直接读取扩展已有位图快照，文件观测允许短暂滞后，不额外扫描或同步等待。

原始输出、文本计划和参数保存在`logs/cold|warm|repeat`。只有SQLerate启用auto_explain，不启用节点ANALYZE/TIMING；不用额外EXPLAIN重跑。结果与本组首次HOST输出比较，FDM只在每条SQL的五个模式完成后检查一次。执行期间只打印进度，不逐条分析性能、不调模型、不重复补测；全部结束后统一读取CSV生成report。脚本和汇总使用sh/awk，不使用Python。

### HOST 缓存对照

给热启动入口设置 `TPCH_WARM_LAYER` 后，只测原生 PG。每轮单独清空两层缓存并执行一次待测 SQL 作预热。`page` 随后重启 PG 以清除 `shared_buffers`，`database` 只清 Guest 文件页缓存，`both` 保留两层。隔离条件指查询开始时的状态，正式执行会正常建立缓存。

```sh
TPCH_WARM_LAYER=page sh /home/femu/tests/tpch-warm.sh -n 3 -o /home/femu/results/host-cache/page
TPCH_WARM_LAYER=database sh /home/femu/tests/tpch-warm.sh -n 3 -o /home/femu/results/host-cache/database
TPCH_WARM_LAYER=both sh /home/femu/tests/tpch-warm.sh -n 1 -o /home/femu/results/host-cache/both
```

每组保留 `warm.csv` 和正式执行的原生 PG 计划。此对照启用 `auto_explain` 的 ANALYZE 与 BUFFERS，关闭逐节点计时。每轮另存 Guest NVMe 块设备计数，`*.nvme-before` 与 `*.nvme-after` 的扇区差可用于区分 PG 缓冲未命中和实际设备读取。

## 选择率校准

`queries/selectivity/1.sql` 至 `22.sql` 与模板一一对应。校准单独运行，传入 0 到 1 的目标比例。

```sh
psql -X -v ON_ERROR_STOP=1 -d tpch_s8 -v target=0.1 -f /home/femu/tests/queries/selectivity/1.sql
```

输出按过滤阶段分行，包括目标与实际选择率、命中行数、输入行数，以及以模板变量名为键的 `parameters` JSON。可以把其中的值作为 `psql -v` 参数传给模板。Q8 的 `q8_nation` 只影响 CASE 聚合值，校准返回 `null`，模板默认使用 `JORDAN`。Q15 的视图名固定为 `revenue`。

- 基表过滤以该表总行数为分母，计入同阶段的固定过滤条件。Q11、Q18 的 HAVING 以分组数为分母。Q19 统计每张表上可单独应用的 OR 分支条件，Q22 分别统计外层区号过滤和内层正余额与区号过滤。
- 独立参数分别取误差最小的值。Q3 和 Q22 的共享参数先使各阶段中最大的绝对误差最小，再比较误差之和。Q11 先选择国家，再校准该国的 HAVING 阈值。
- 日期窗口与可调数值阈值可以超出 QGEN 的标准随机生成范围。固定窗口、离散类别和分组结构会限制能达到的选择率，返回候选中最接近的实际值。使用默认模板参数的专项组件测试仍可通过 `tpch-parameters.psql` 加载。

在仓库宿主机运行以下命令，从 Guest 的 `tpch_s8` 校准并生成 `tests/guest/queries/sf8_sel0.1/1.sql` 至 `22.sql`。

```sh
sh tests/guest/queries/selectivity/generate.sh 0.1
```

第二个位置参数或环境变量 `SF` 可指定其他规模，例如 `sh tests/guest/queries/selectivity/generate.sh 0.1 4` 生成 `sf4_sel0.1/`。

生成的 SQL 已填入具体参数，文件开头记录各过滤阶段的实际选择率。若目标目录已存在，脚本会退出，避免覆盖已有文件。
