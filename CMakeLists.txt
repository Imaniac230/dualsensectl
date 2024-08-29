cmake_minimum_required(VERSION 3.5)
project(dualsensectl VERSION 0.6 LANGUAGES C)

# Default to C99
if (NOT CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 99)
endif ()

if (CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif ()

set(CORE_LIB_NAME lib${PROJECT_NAME})
set(EXECUTABLE_NAME ${PROJECT_NAME})
add_compile_definitions(DUALSENSECTL_VERSION="${CMAKE_PROJECT_VERSION}")

include(FindPkgConfig)
pkg_check_modules(dbus REQUIRED dbus-1)
#find_package(hidapi-hidraw REQUIRED)
#find_package(libudev REQUIRED)

include_directories(include)

add_library(${CORE_LIB_NAME} STATIC src/Dualsense.c)
target_link_libraries(${CORE_LIB_NAME} dbus-1 hidapi-hidraw udev)
target_include_directories(${CORE_LIB_NAME} PUBLIC include ${dbus_INCLUDE_DIRS})

add_executable(${EXECUTABLE_NAME} src/main.c)
target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${CORE_LIB_NAME})
target_include_directories(${EXECUTABLE_NAME} PUBLIC include ${dbus_INCLUDE_DIRS})
