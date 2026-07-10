include("../cmake/Findthrift.cmake")

find_package(nlohmann_json 3.5.0 REQUIRED)
find_package(Threads)

set(THRIFT_GEN_CPP_DIR ../gen-cpp)
set(NU_DIR ../../../../)
set(CALADAN_DIR ${NU_DIR}/caladan/)
set(CMAKE_EXE_LINKER_FLAGS "-T ${CALADAN_DIR}/base/base.ld")

execute_process(COMMAND nproc OUTPUT_VARIABLE NCORES)
add_compile_definitions(NCORES=${NCORES})
add_compile_definitions(DDB_SUPPORT)

add_executable(
    main
    main.cpp
    utils.cpp
    states.cpp
    ThriftBackEndServer.cpp
    BackEndService.cpp
    ${THRIFT_GEN_CPP_DIR}/BackEndService.cpp
    ${THRIFT_GEN_CPP_DIR}/social_network_types.cpp
)

# DDB headers are installed by `make install` in connector/, not vendored in Nu.
set(DDB_INCLUDE_DIR "$ENV{HOME}/.local/include" CACHE PATH "DDB connector headers")

target_include_directories(
    main PRIVATE
    ../third_party
    ${THRIFT_INCLUDE_DIR}
    ${NU_DIR}/inc
    ${CALADAN_DIR}/inc
    ${CALADAN_DIR}/bindings/cc
    ${DDB_INCLUDE_DIR}
)

target_link_directories(
    main PRIVATE
    ${CALADAN_DIR}
    ${CALADAN_DIR}/bindings/cc
    ${CALADAN_DIR}/rdma-core/build/lib/
    ${NU_DIR}
)

target_link_libraries(
    main
    ${THRIFT_STATIC_LIB}
    ${CMAKE_THREAD_LIBS_INIT}
    nlohmann_json::nlohmann_json
    # Nu
    nu
    crypto
    boost_program_options
    numa
    # Caladan
    rt++
    runtime
    net
    base
    mlx5
    ibverbs
    nl-3
    nl-route-3
    pthread
    dl
    # DDB Dependency
    paho-mqtt3c
)
