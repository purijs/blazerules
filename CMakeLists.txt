cmake_minimum_required(VERSION 3.22)
project(blazerules VERSION 0.1.0 LANGUAGES CXX)
include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

message(STATUS "CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
message(STATUS "CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")

option(BLAZERULES_NATIVE_TUNE "Tune release builds for the local CPU where safe; use OFF for distributable/cloud-portable builds" ON)
option(BLAZERULES_X86_AVX2 "Build AVX2/FMA runtime-dispatched kernels for x86_64" ON)
option(BLAZERULES_X86_AVX512 "Build optional AVX-512 runtime-dispatched kernels for x86_64" ON)

if(MSVC)
        add_compile_options(/W4 /permissive-)
        add_compile_options($<$<CONFIG:Debug>:/Od>)
        add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Ob3> $<$<CONFIG:Release>:/DNDEBUG>)
        add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
else()
        add_compile_options(-Wall -Wextra -Wpedantic)
        add_compile_options($<$<CONFIG:Debug>:-O0>)
        add_compile_options($<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-DNDEBUG>)

        check_cxx_compiler_flag("-mcpu=apple-m1" HAS_APPLE_MCPU)
        if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" AND HAS_APPLE_MCPU)
                add_compile_options($<$<CONFIG:Release>:-mcpu=apple-m1>)
        elseif(UNIX AND NOT APPLE AND BLAZERULES_NATIVE_TUNE)
                check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
                if(HAS_MARCH_NATIVE)
                        add_compile_options($<$<CONFIG:Release>:-march=native>)
                endif()
        endif()

endif()

check_ipo_supported(RESULT BLAZERULES_IPO_SUPPORTED OUTPUT BLAZERULES_IPO_ERROR)
if(BLAZERULES_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG QUIET)
find_package(simdjson CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)

set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)

add_library(blazerules_core STATIC
        src/compiler/dsl_parser.cpp
        src/compiler/sql_expr_parser.cpp
        src/compiler/schema.cpp
        src/compiler/compiler.cpp
        src/compiler/conflict.cpp
        src/core/kernels.cpp
        src/core/transposer.cpp
        src/core/dict_encoder.cpp
        src/core/resource_resolver.cpp
        src/core/schema_inference.cpp
        src/core/window_store.cpp
        src/core/model_registry.cpp
        src/core/vector_channels.cpp
        src/core/mmap_reader.cpp
        src/core/orchestrator.cpp
        src/core/engine.cpp
)

target_include_directories(blazerules_core
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(BLAZERULES_IS_X86_64 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(BLAZERULES_IS_X86_64 TRUE)
endif()

if(BLAZERULES_IS_X86_64 AND BLAZERULES_X86_AVX2)
        if(MSVC)
                target_sources(blazerules_core PRIVATE src/core/kernels_avx2.cpp)
                set_source_files_properties(src/core/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
                target_compile_definitions(blazerules_core PRIVATE BLAZERULES_BUILD_AVX2=1)
                message(STATUS "BlazeRules: AVX2 runtime kernels ENABLED (MSVC /arch:AVX2)")
                if(BLAZERULES_X86_AVX512)
                        target_sources(blazerules_core PRIVATE src/core/kernels_avx512.cpp)
                        set_source_files_properties(src/core/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
                        target_compile_definitions(blazerules_core PRIVATE BLAZERULES_BUILD_AVX512=1)
                        message(STATUS "BlazeRules: AVX-512 runtime kernels ENABLED (MSVC /arch:AVX512)")
                endif()
        else()
                check_cxx_compiler_flag("-mavx2" HAS_AVX2_FLAG)
                check_cxx_compiler_flag("-mfma" HAS_FMA_FLAG)
                check_cxx_compiler_flag("-mbmi2" HAS_BMI2_FLAG)
                check_cxx_compiler_flag("-mpopcnt" HAS_POPCNT_FLAG)
                check_cxx_compiler_flag("-mlzcnt" HAS_LZCNT_FLAG)
                if(HAS_AVX2_FLAG AND HAS_FMA_FLAG)
                        target_sources(blazerules_core PRIVATE src/core/kernels_avx2.cpp)
                        set(BLAZERULES_AVX2_FLAGS -mavx2 -mfma)
                        if(HAS_BMI2_FLAG)
                                list(APPEND BLAZERULES_AVX2_FLAGS -mbmi2)
                        endif()
                        if(HAS_POPCNT_FLAG)
                                list(APPEND BLAZERULES_AVX2_FLAGS -mpopcnt)
                        endif()
                        if(HAS_LZCNT_FLAG)
                                list(APPEND BLAZERULES_AVX2_FLAGS -mlzcnt)
                        endif()
                        set_source_files_properties(src/core/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "${BLAZERULES_AVX2_FLAGS}")
                        target_compile_definitions(blazerules_core PRIVATE BLAZERULES_BUILD_AVX2=1)
                        message(STATUS "BlazeRules: AVX2 runtime kernels ENABLED (${BLAZERULES_AVX2_FLAGS})")
                        if(BLAZERULES_X86_AVX512)
                                check_cxx_compiler_flag("-mavx512f" HAS_AVX512F_FLAG)
                                check_cxx_compiler_flag("-mavx512bw" HAS_AVX512BW_FLAG)
                                check_cxx_compiler_flag("-mavx512vl" HAS_AVX512VL_FLAG)
                                check_cxx_compiler_flag("-mavx512dq" HAS_AVX512DQ_FLAG)
                                check_cxx_compiler_flag("-mavx512vpopcntdq" HAS_AVX512VPOPCNTDQ_FLAG)
                                if(HAS_AVX512F_FLAG AND HAS_AVX512BW_FLAG AND HAS_AVX512VL_FLAG AND HAS_AVX512DQ_FLAG)
                                        target_sources(blazerules_core PRIVATE src/core/kernels_avx512.cpp)
                                        set(BLAZERULES_AVX512_FLAGS -mavx512f -mavx512bw -mavx512vl -mavx512dq -mfma)
                                        if(HAS_AVX512VPOPCNTDQ_FLAG)
                                                list(APPEND BLAZERULES_AVX512_FLAGS -mavx512vpopcntdq)
                                        endif()
                                        set_source_files_properties(src/core/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "${BLAZERULES_AVX512_FLAGS}")
                                        target_compile_definitions(blazerules_core PRIVATE BLAZERULES_BUILD_AVX512=1)
                                        message(STATUS "BlazeRules: AVX-512 runtime kernels ENABLED (${BLAZERULES_AVX512_FLAGS})")
                                else()
                                        message(STATUS "BlazeRules: AVX-512 runtime kernels DISABLED (compiler lacks required flags)")
                                endif()
                        endif()
                else()
                        message(STATUS "BlazeRules: AVX2 runtime kernels DISABLED (compiler lacks -mavx2/-mfma)")
                endif()
        endif()
endif()

target_link_libraries(blazerules_core
        PUBLIC
        Arrow::arrow_static
        simdjson::simdjson
        absl::flat_hash_map
        TBB::tbb
        yaml-cpp::yaml-cpp
        re2::re2
)

if(TARGET Parquet::parquet_static)
        target_link_libraries(blazerules_core PUBLIC Parquet::parquet_static)
elseif(TARGET Parquet::parquet_shared)
        target_link_libraries(blazerules_core PUBLIC Parquet::parquet_shared)
elseif(TARGET parquet_static)
        target_link_libraries(blazerules_core PUBLIC parquet_static)
endif()

option(BLAZERULES_ENABLE_ONNX "Embedded ONNX Runtime ML scoring (model_score)" ON)
if(BLAZERULES_ENABLE_ONNX)
        find_package(onnxruntime CONFIG REQUIRED)
        target_link_libraries(blazerules_core PUBLIC onnxruntime::onnxruntime)
        target_compile_definitions(blazerules_core PUBLIC BLAZERULES_ENABLE_ONNX)
        message(STATUS "BlazeRules: ONNX Runtime ML scoring ENABLED")
else()
        message(STATUS "BlazeRules: ONNX Runtime ML scoring DISABLED (lean build)")
endif()

add_executable(blazerules_driver src/driver/main.cpp)
target_link_libraries(blazerules_driver PRIVATE blazerules_core)

pybind11_add_module(blazerules MODULE src/bindings/pybind_module.cpp)
target_link_libraries(blazerules PRIVATE blazerules_core)
install(TARGETS blazerules
        LIBRARY DESTINATION .
        RUNTIME DESTINATION .
        COMPONENT python)

option(BLAZERULES_IO "Build streaming/IO connectors (Kafka, CDC, decoders)" ON)
option(BLAZERULES_IO_KAFKA "Kafka source/sink via librdkafka (needs the io module)" ON)
option(BLAZERULES_IO_AVRO "Avro binary decoder in blazerules_io" ON)
option(BLAZERULES_IO_PROTOBUF "Protobuf dynamic descriptor decoder in blazerules_io" ON)
option(BLAZERULES_IO_S3 "Compatibility flag; exact-object s3:// IO uses the AWS CLI cache path" ON)
if(BLAZERULES_IO)
        set(BLAZERULES_IO_SOURCES
                src/io/cdc.cpp
                src/io/file_reader.cpp
                src/io/decoders/common.cpp
                src/io/decoders/arrow_ipc.cpp
        )
        if(BLAZERULES_IO_KAFKA)
                find_package(RdKafka CONFIG QUIET)
                if(NOT TARGET RdKafka::rdkafka++)
                        find_package(unofficial-librdkafka CONFIG QUIET)
                endif()
                list(APPEND BLAZERULES_IO_SOURCES src/io/kafka.cpp)
                list(APPEND BLAZERULES_IO_SOURCES src/io/stream_runtime.cpp)
        endif()
        if(BLAZERULES_IO_AVRO)
                find_package(fmt CONFIG REQUIRED)
                find_package(ZLIB REQUIRED)
                find_path(AVRO_CPP_INCLUDE_DIR avro/Compiler.hh REQUIRED)
                find_library(AVRO_CPP_LIBRARY NAMES avrocpp_s REQUIRED)
                list(APPEND BLAZERULES_IO_SOURCES src/io/decoders/avro.cpp)
        endif()
        if(BLAZERULES_IO_PROTOBUF)
                find_package(protobuf CONFIG REQUIRED)
                list(APPEND BLAZERULES_IO_SOURCES src/io/decoders/protobuf.cpp)
        endif()
        add_library(blazerules_io STATIC ${BLAZERULES_IO_SOURCES})
        target_link_libraries(blazerules_io PUBLIC blazerules_core)
        target_include_directories(blazerules_io PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
        if(BLAZERULES_IO_KAFKA)
                if(TARGET RdKafka::rdkafka++)
                        target_link_libraries(blazerules_io PUBLIC RdKafka::rdkafka++)
                elseif(TARGET unofficial::librdkafka::librdkafka)
                        target_link_libraries(blazerules_io PUBLIC unofficial::librdkafka::librdkafka)
                endif()
                target_compile_definitions(blazerules_io PUBLIC BLAZERULES_IO_KAFKA)
        endif()
        if(BLAZERULES_IO_AVRO)
                target_include_directories(blazerules_io PUBLIC ${AVRO_CPP_INCLUDE_DIR})
                target_link_libraries(blazerules_io PUBLIC ${AVRO_CPP_LIBRARY} fmt::fmt ZLIB::ZLIB)
                target_compile_definitions(blazerules_io PUBLIC BLAZERULES_IO_AVRO)
        endif()
        if(BLAZERULES_IO_PROTOBUF)
                target_link_libraries(blazerules_io PUBLIC protobuf::libprotobuf)
                target_compile_definitions(blazerules_io PUBLIC BLAZERULES_IO_PROTOBUF)
        endif()
        if(BLAZERULES_IO_S3)
                target_compile_definitions(blazerules_io PUBLIC BLAZERULES_IO_S3)
        endif()

        pybind11_add_module(blazerules_io_py MODULE src/bindings/pybind_io_module.cpp)
        set_target_properties(blazerules_io_py PROPERTIES OUTPUT_NAME blazerules_io)
        target_link_libraries(blazerules_io_py PRIVATE blazerules_io)
        install(TARGETS blazerules_io_py
                LIBRARY DESTINATION .
                RUNTIME DESTINATION .
                COMPONENT python)
        message(STATUS "BlazeRules: IO connectors ENABLED (kafka=${BLAZERULES_IO_KAFKA}, avro=${BLAZERULES_IO_AVRO}, protobuf=${BLAZERULES_IO_PROTOBUF}, s3=${BLAZERULES_IO_S3})")
endif()

option(BLAZERULES_DASHBOARD "Build the local read-only BlazeRules dashboard server" ON)
if(BLAZERULES_DASHBOARD)
        find_package(httplib CONFIG REQUIRED)
        add_executable(blazerules_dashboard src/dashboard/main.cpp)
        target_include_directories(blazerules_dashboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(blazerules_dashboard PRIVATE httplib::httplib)
        install(TARGETS blazerules_dashboard RUNTIME DESTINATION bin COMPONENT python)
        message(STATUS "BlazeRules: dashboard server ENABLED")
endif()

option(BLAZERULES_AGENT "Build the BlazeRules local multi-instance ingest agent" ON)
if(BLAZERULES_AGENT)
        find_package(httplib CONFIG REQUIRED)
        add_executable(blazerules_agent src/agent/main.cpp)
        target_include_directories(blazerules_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(blazerules_agent PRIVATE blazerules_core httplib::httplib)
        install(TARGETS blazerules_agent RUNTIME DESTINATION bin COMPONENT python)
        message(STATUS "BlazeRules: agent ENABLED")
endif()

install(TARGETS blazerules_core
        EXPORT blazerulesTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        COMPONENT dev)

install(DIRECTORY include/blazerules DESTINATION include COMPONENT dev)

install(EXPORT blazerulesTargets
        NAMESPACE blazerules::
        DESTINATION lib/cmake/blazerules
        COMPONENT dev)
