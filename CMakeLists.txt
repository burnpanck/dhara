cmake_minimum_required(VERSION 3.13)

project(dhara-nand-ftl C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_VERBOSE_MAKEFILE ON)

add_library(dhara-cpp
        src/error.cpp
        src/journal.cpp
#        src/map.cpp
        )
target_include_directories(dhara-cpp PUBLIC include/)

add_library(testlib
        tests-c++/sim.cpp
        tests-c++/util.cpp
        tests-c++/jtutil.cpp
)
target_link_libraries(testlib PUBLIC dhara-cpp)

set(tests
        epoch_roll
        error
        jfill
        journal
        map
        nand
        recovery
    )

foreach(test ${tests})
    add_executable(${test}-c++.test
            tests-c++/${test}.cpp
            )
    target_link_libraries(${test}-c++.test PRIVATE dhara-cpp testlib)
endforeach()

# include_directories(${CMAKE_CURRENT_LIST_DIR})
# add_compile_options(-O1 -std=c11 -Wall -Wextra -ggdb)

add_library(dhara
        dhara/error.c
        dhara/journal.c
        dhara/map.c
        )
target_include_directories(dhara PUBLIC .)

add_library(ecc
        ecc/bch.c
        ecc/crc32.c
        ecc/gf13.c
        ecc/hamming.c
        )

add_library(testutils
        tests/sim.c
        tests/util.c
        tests/jtutil.c
        )
target_link_libraries(testutils PUBLIC dhara ecc)


if(FALSE)
add_library(testutils2
        tests/sim2.c
        tests/util.c
        tests/jtutil.c
        )
target_link_libraries(testutils2 PUBLIC dhara ecc)
endif()


set(tests
    bch
    crc32
    epoch_roll
    error
    hamming
    jfill
    journal
    map
#    map2
    nand
    recovery
    read_what_is_written
)

foreach(test ${tests})
add_executable(${test}.test
        tests/${test}.c
        )
target_link_libraries(${test}.test PRIVATE dhara ecc testutils)
endforeach()

add_executable(gentab
        tools/gentab.c
        )
target_link_libraries(gentab dhara ecc)

add_executable(gftoot
        tools/gftool.c
        )
target_link_libraries(gftoot dhara ecc)
