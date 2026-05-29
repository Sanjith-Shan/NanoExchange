# Every bench_*.cpp is compiled into one benchmarks binary. The files register
# their cases with the BENCHMARK macro and provide no main of their own. The
# benchmark_main target supplies main, so ./build/bin/benchmarks runs them all
# and --benchmark_filter selects a subset.
file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp)

if(BENCH_SOURCES)
    add_executable(benchmarks ${BENCH_SOURCES})
    target_link_libraries(benchmarks PRIVATE nano_core benchmark::benchmark_main)
    set_target_properties(benchmarks PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
