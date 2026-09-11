cmake_minimum_required(VERSION 3.16)
project(zenoh_cpp_vendor)

# Default to C++17
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_vendor_package REQUIRED)

option(USE_SYSTEM_ZENOH "Use non-vendored zenoh from the system" OFF)

if(USE_SYSTEM_ZENOH)
  find_package(zenohc REQUIRED)
  find_package(zenohcxx REQUIRED)
  message(STATUS "Using system zenohc version: ${zenohc_VERSION}")
  message(STATUS "Using system zenohcxx version: ${zenohcxx_VERSION}")
else()
  # Add a set of usefull features that are not enabled by default in zenoh-c.
  # For a complete list of features see: https://github.com/eclipse-zenoh/zenoh/blob/main/zenoh/Cargo.toml
  # Note: the list of features are separated with whitespaces.
  # However, if you want to add other cargo build flags, use "$<SEMICOLON>" as separator and not ";" as the
  # latter is a list separater in cmake and hence the string will be split into two when expanded.
  set(ZENOHC_CARGO_FLAGS "--features=shared-memory zenoh/transport_serial")

  include(get_cargo_version.cmake)
  get_cargo_version()

  # CONTROL VARIANT of dexory/fixes-1.10.1: upstream fixes only, none of the
  # fork-only patches, to check whether those are still needed on zenoh 1.10.1.
  #
  # botsandus/zenoh-c@dexory/upstream-only-1.10.1 = upstream zenoh-c 1.10.1 with
  # the Cargo manifests/locks pointing zenoh/zenoh-ext/zenoh-runtime/zenoh-util at
  # botsandus/zenoh@dexory/upstream-only-1.10.1 = zenoh 1.10.1 plus ONLY:
  # - upstream PR #2779 (merged to main after 1.10.1 was cut): StartConditions
  #   made sync, both block_in_place-under-ctrl_lock sites dropped -- the
  #   gossip/OAM deadlock behind the random bringup freezes
  #   (https://github.com/eclipse-zenoh/zenoh/issues/2581);
  # - upstream PR #2744 (still open): zenoh-ext AdvancedSubscriber and
  #   QueryingSubscriber no longer call user code while holding their state lock.
  # NOT carried here (present on dexory/fixes-1.10.1): the SyncGroup
  # undeclare-from-callback fix, the Mux request-finalize fix, the zenoh-ext
  # history-retry watchdog, and this package's service-request retry layer.
  set(zenoh_c_commit 4404841c9ef49e9f3d0b60485d9975d33a388a7c)

  # zenoh 1.10 requires Rust >= 1.88. The upstream 1.75 fallback commit is a
  # 1.8-era zenoh-c with none of the fixes above and an incompatible wire/SHM
  # format, so fail loudly instead of silently mixing versions.
  if(CARGO_VERSION VERSION_LESS "1.88")
    message(FATAL_ERROR "dexory/upstream-only-1.10.1 requires Rust >= 1.88 (zenoh 1.10 MSRV); found ${CARGO_VERSION}")
  endif()

  ament_vendor(zenoh_c_vendor
    VCS_URL https://github.com/botsandus/zenoh-c.git
    VCS_VERSION ${zenoh_c_commit}
    CMAKE_ARGS
      "-DZENOHC_CARGO_FLAGS=${ZENOHC_CARGO_FLAGS}"
      "-DZENOHC_BUILD_WITH_UNSTABLE_API=TRUE"
      "-DZENOHC_CUSTOM_TARGET=${ZENOHC_CUSTOM_TARGET}"
  )

  ament_vendor(zenoh_cpp_vendor
    VCS_URL https://github.com/eclipse-zenoh/zenoh-cpp
    # zenoh-cpp 1.10.1 (tag), matching zenoh-c 1.10.1
    VCS_VERSION 9626af8baa7535b6cea599a9cd56a7fc200a176b
    CMAKE_ARGS
      -DZENOHCXX_ZENOHC=OFF
  )

  externalproject_add_stepdependencies(zenoh_cpp_vendor configure zenoh_c_vendor)
endif()

ament_export_dependencies(zenohc)
ament_export_dependencies(zenohcxx)

ament_package()
