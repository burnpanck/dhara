cmake_minimum_required(VERSION 3.13)

project(dhara-nand-ftl C)

set(CMAKE_C_STANDARD 99)

include_directories(${CMAKE_CURRENT_LIST_DIR})

add_library(dhara
        dhara/error.c
        dhara/journal.c
        dhara/map.c
        )

target_compile_options(dhara PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)

add_library(ecc
        ecc/bch.c
        ecc/crc32.c
        ecc/gf13.c
        ecc/hamming.c
        )

target_compile_options(ecc PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)

add_library(testutils
        tests/sim.c
        tests/util.c
        tests/jtutil.c
        )
target_compile_options(testutils PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(testutils dhara ecc)


add_executable(bch.test
        tests/bch.c
        )
target_compile_options(bch.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(bch.test dhara ecc testutils)

add_executable(crc32.test
        tests/crc32.c
        )
target_compile_options(crc32.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(crc32.test dhara ecc testutils)

add_executable(epoch_roll.test
        tests/epoch_roll.c
        )
target_compile_options(epoch_roll.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(epoch_roll.test dhara ecc testutils)

add_executable(error.test
        tests/error.c
        )
target_compile_options(error.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(error.test dhara ecc testutils)

add_executable(hamming.test
        tests/hamming.c
        )
target_compile_options(hamming.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(hamming.test dhara ecc testutils)

add_executable(jfill.test
        tests/jfill.c
        )
target_compile_options(jfill.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(jfill.test dhara ecc testutils)

add_executable(journal.test
        tests/journal.c
        )
target_compile_options(journal.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(journal.test dhara ecc testutils)

add_executable(map.test
        tests/map.c
        )
target_compile_options(map.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(map.test dhara ecc testutils)

add_executable(nand.test
        tests/nand.c
        )
target_compile_options(nand.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(nand.test dhara ecc testutils)

add_executable(recovery.test
        tests/recovery.c
        )
target_compile_options(recovery.test  PUBLIC -O1 -std=c99 -Wall -Wextra -ggdb)
target_link_libraries(recovery.test dhara ecc testutils)
