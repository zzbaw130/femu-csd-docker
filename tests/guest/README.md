# TPC-H 全量测试

在Guest中使用三个入口，均测试五种exe_path，默认读取本目录`queries/1.sql`至`22.sql`。目录沿用仓库已有的`tests/guest`，Guest中挂载为`/home/femu/tests`。

```sh
export PGDATA=/data/tpch_s4 PGDATABASE=tpch_s4
sh /home/femu/tests/tpch-cold.sh   -n 3 -o /home/femu/results/tpch-sf4
sh /home/femu/tests/tpch-warm.sh   -n 3 -o /home/femu/results/tpch-sf4
sh /home/femu/tests/tpch-repeat.sh -n 5 -o /home/femu/results/tpch-sf4
```

参数：`-n`测试次数，`-b`shared_buffers（默认2GB），`-w`max_parallel_workers_per_gather（默认4），`-m`work_mem（默认16MB），`-D`PGDATA，`-d`数据库，`-o`结果目录。内存也接受`2G`、`16M`写法。全局parallel_workers/worker_processes至少为8或指定worker数，以免限制每个Gather的配置。可在选项后指定少量查询编号作脚本检查，例如`-n 2 6 15`；正式全量不传编号。

- 冷启动：每次测量前停止PG、sync、清Guest page cache、重启PG。
- 热启动：每条SQL、每种模式先清两层缓存，原生HOST执行该SQL一次，再在同一连接连续测n次。预热不计时。
- 重复启动：每条SQL、每种模式从冷态直接连续执行n次，不做HOST预热；相邻执行停顿1秒给异步观测缓冲，停顿不计查询耗时。

切换SQL或exe_path时均隔离缓存。前三个模式关闭HPCP/DCP，`csd-with-hpcp`和`SQLerate`同时开启。连接初始化及CSF加载在正式计时之外；不在组内运行采样SQL。`tpch-test.sh`只保存公共执行和CSV整理，三个入口复用它，不维护三套重复实现。

每次全量完成产生两份CSV，三个入口合计六份：

| 文件 | 内容 |
| --- | --- |
| `cold.csv`、`warm.csv`、`repeat.csv` | 每行Q1-1这样的编号，五种模式原始毫秒数、各自ExecutorEnd维护时间、正确性及FDM状态 |
| `cold-sqlerate.csv`、`warm-sqlerate.csv`、`repeat-sqlerate.csv` | 每次SQLerate的规划、执行、脚本总时间、ExecutorEnd维护时间、缓存驻留和实际路径 |

每份CSV恰好22×n条记录，不提前求中位数。错误也保留该行的原始时间和失败状态；`checks`应全部为ok，`fdm_clean`应为yes，SQLerate详情status应为ok。Q15总时间包括CREATE/SELECT/DROP，执行时间为主SELECT的执行器计时，因此不能强求规划+执行等于脚本总时间。

`*_ms`和`total_ms`是原始psql时间。按项目既定口径分析查询主体时，减去对应`*_executor_end_ms`；原始值不可覆盖。热态和重复CSV还保存五种模式的`*_cache_residency`：`shared`、`file`、`known`分别为已观测shared_buffers驻留、文件缓存驻留、文件观测覆盖百分比。分母是当前数据库用户表及索引main fork总页数，**不是当前SQL工作集命中率**，两层缓存不可相加。数据直接读取扩展已有位图快照，文件观测允许短暂滞后，不额外扫描或同步等待。

原始输出、文本计划和参数保存在`logs/cold|warm|repeat`。只有SQLerate启用auto_explain，不启用节点ANALYZE/TIMING；不用额外EXPLAIN重跑。结果与本组首次HOST输出比较，FDM只在每条SQL的五个模式完成后检查一次。执行期间只打印进度，不逐条分析性能、不调模型、不重复补测；全部结束后统一读取CSV生成report。脚本和汇总使用sh/awk，不使用Python。
