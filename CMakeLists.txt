cmake_minimum_required(VERSION 3.20)

project(electronic_order_book)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(
        electronic_order_book
        main.cpp
)

target_include_directories(
        electronic_order_book
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(
            electronic_order_book
            PRIVATE
            ws2_32
            psapi
    )
endif()