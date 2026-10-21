cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(MacroSwitcher VERSION 1.0.0)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK ortam degiskeni bulunamadi! Geode SDK yolunu tanimlayin.")
else()
    message(STATUS "Geode bulundu: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# GDR (.gdr / .gdr2) replay formati kutuphanesi
CPMAddPackage("gh:maxnut/GDReplayFormat#main")
target_link_libraries(${PROJECT_NAME} libGDR)

setup_geode_mod(${PROJECT_NAME})
