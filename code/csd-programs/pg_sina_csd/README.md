# pg_sina_csd 设备算子

这里的 .so 在容器内编译，由 FEMU 的 CU 线程执行，逻辑上属于 CSD CPU。Guest 扩展和 SNIA 调度代码位于 code/apps/pg_sina_csd；FEMU 不解释 SQL。

当前 heap_count.c 编译为 build/pg-sina-heap-count.so，导出 pg_sina_heap_count。MR[0]为PG12原始heap页，MR[1]为pg_sina_count输出；cparam1为物理列数，cparam2为有效输入长度。只处理x86-64小端、8KiB页、完全visible/frozen的heap；格式或可见性不符合时返回负值，SNIA报告执行错误。

本入口只计数，无跨调用私有状态。后续有私有算法工作区的算子必须使用libcs-workmem；构建已通过根目录add_csd_program统一链接。AFDM只存输入/输出，不作为算法运行空间。

```sh
cmake -S code/csd-programs -B code/csd-programs/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build code/csd-programs/build -j2
ctest --test-dir code/csd-programs/build -R pgSina --output-on-failure
```

生产请求由Guest构建LOAD→count的SNIA batch。设备算子不调用NVMe，不持有Guest文件描述符，不自行访问SQL元数据。
