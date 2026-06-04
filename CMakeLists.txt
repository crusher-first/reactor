cmake_minimum_required(VERSION 3.10)
project(HighPerformanceWebServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra")

# 查找 liburing
find_package(PkgConfig)
pkg_check_modules(LIBURING REQUIRED liburing)

# 头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${LIBURING_INCLUDE_DIRS}
)

# 源文件
set(SOURCES
    main.cpp
    src/server.cpp
    src/logger.cpp
)

# 可执行文件
add_executable(server ${SOURCES})

# 编译选项
target_compile_options(server PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -O3 -march=native
)

# 链接
target_link_libraries(server PRIVATE
    ${LIBURING_LIBRARIES}
    pthread
)

target_link_options(server PRIVATE -O3 -Wl,-rpath,.)

# 安装
install(TARGETS server DESTINATION bin)

# 打印配置信息
message(STATUS "===================================")
message(STATUS "High-Performance C++ Web Server")
message(STATUS "===================================")
message(STATUS "liburing include: ${LIBURING_INCLUDE_DIRS}")
message(STATUS "liburing library: ${LIBURING_LIBRARIES}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "===================================")
