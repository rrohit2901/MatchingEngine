# bench/CMakeLists.txt
# One executable per operation (mirrors tests/CMakeLists.txt's one-exe-per-file convention).
# Each links Google Benchmark's benchmark_main (provides main()) and project_options for the
# Release perf flags. project_warnings is intentionally omitted: the strict warning set
# (-Wconversion, -Wold-style-cast, ...) trips on the benchmark glue/headers.

function(add_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE
        me_core
        benchmark::benchmark_main
        project_options
    )
endfunction()

add_bench(bench_add)
add_bench(bench_modify)
add_bench(bench_cancel)

# Per-operation latency harness (P50/P99/P99.9). Standalone — it has its own main(),
# so it does NOT link benchmark_main; it only needs the core library.
add_executable(bench_latency bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE
    me_core
    me_ds_lock          # LockQueue for the MatchingEngine event container
    Threads::Threads    # background event drain
    project_options
)
