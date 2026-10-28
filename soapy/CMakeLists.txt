# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Phenix-1255 contributors

cmake_minimum_required(VERSION 3.16)
project(SoapyPhenix1255 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SoapySDR "0.8" CONFIG REQUIRED)
find_package(Threads REQUIRED)

# HS USB transport, optional (libusb-1.0 >= 1.0.21)
option(PHX_ENABLE_USB "Build the HS USB transport" ON)
if(PHX_ENABLE_USB)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0>=1.0.21)
  endif()
  if(NOT LIBUSB_FOUND)
    message(WARNING "libusb-1.0 not found, building without USB support")
  endif()
endif()

SOAPY_SDR_MODULE_UTIL(
  TARGET phenix1255Support
  SOURCES
    src/ControlClient.cpp
    src/Phenix1255Device.cpp
    src/Registration.cpp
    src/SampleStream.cpp
    src/TransportNet.cpp
  LIBRARIES
    Threads::Threads
)

target_include_directories(phenix1255Support PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)
target_compile_options(phenix1255Support PRIVATE -Wall -Wextra)

if(LIBUSB_FOUND)
  target_sources(phenix1255Support PRIVATE src/TransportUsb.cpp)
  target_compile_definitions(phenix1255Support PRIVATE PHX_HAVE_USB=1)
  target_link_libraries(phenix1255Support PRIVATE PkgConfig::LIBUSB)
endif()

# Test programs (not installed)
add_executable(phx_soapy_test tests/phx_soapy_test.cpp)
target_link_libraries(phx_soapy_test PRIVATE SoapySDR Threads::Threads)
add_executable(phx_loopback_test tests/phx_loopback_test.cpp)
target_link_libraries(phx_loopback_test PRIVATE SoapySDR)
