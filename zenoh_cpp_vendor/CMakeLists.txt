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

  # botsandus/zenoh-c@dexory/fixes-1.10.0 = upstream zenoh-c main (post-1.9,
  # synced with zenoh@3118e85) with Cargo manifests/locks pointing
  # zenoh/zenoh-ext/zenoh-runtime/zenoh-util at
  # botsandus/zenoh@dexory/fixes-1.10.0 = zenoh 1.10.0 plus:
  # - the gossip/OAM ctrl_lock deadlock fix (StartConditions sync, both
  #   block_in_place sites dropped — https://github.com/eclipse-zenoh/zenoh/issues/2581,
  #   upstream PR #2637 still open);
  # - the undeclare-from-callback self-deadlock fix (SyncGroup::wait called on
  #   the rx thread from ~SubscriptionData via ze_undeclare_advanced_subscriber
  #   parks the rx thread forever): drains callbacks asynchronously in that case;
  # - finalization of queries whose request fails transport scheduling
  #   (closed/closing link under churn) so service calls fail fast and
  #   retryably instead of hanging their full timeout;
  # - gossip linkstates advertising loopback locators again (regions redesign
  #   #2096 dropped them): without this, peers listening only on loopback —
  #   this package's default session config — never form the same-host mesh on
  #   zenoh >= 1.10 and have NO data path (routers no longer broker data
  #   between their south-bound peers);
  # - zenoh-ext history-retry: an AdvancedSubscriber re-queries an alive
  #   publisher's undelivered TRANSIENT_LOCAL history with capped backoff
  #   instead of giving up after the single initial query is lost under
  #   transport churn (the fail-fast finalization above only makes that loss
  #   prompt; nothing re-asked, so e.g. a 73MB latched map replayed to rviz
  #   at startup arrived "in ~5s or never"). Heartbeat recovery cannot cover
  #   this: a sporadic heartbeat only follows a *new* publication.
  # The 1.8-era query-no-shm patch is intentionally DROPPED: zenoh 1.10.0
  # ships the SHM handoff (eclipse-zenoh/zenoh#2646) which fixes the
  # underlying #2628 in-flight SHM invalidation.
  set(zenoh_c_commit 246857534e20cb07687985ac3cc5b228c16b406f)

  # If Rust version below Zenoh MSRV (currently 1.88), use equivalent commit from branch ROS/rust-1.75-zenoh-2687c5135
  # See https://github.com/eclipse-zenoh/zenoh-c/blob/b31348fa7f94f44f1f7b049c111a710e970a2725/Cargo.lock#L4237
  # pointing to zenoh https://github.com/eclipse-zenoh/zenoh/commit/2687c51352121f006e3a603ce07925a8ad0b295c
  # NOTE: this 1.8-era fallback carries NONE of the 1.10 fixes above and is
  # unsupported for the dexory/fixes-1.10.0 line (zenoh 1.10 wire/SHM format
  # is incompatible anyway) — kept only so the gate fails loudly at link time
  # rather than silently mixing versions.
  if(CARGO_VERSION VERSION_GREATER_EQUAL "1.75" AND CARGO_VERSION VERSION_LESS "1.88")
    message(FATAL_ERROR "dexory/fixes-1.10.0 requires Rust >= 1.88 (zenoh 1.10 MSRV); found ${CARGO_VERSION}")
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
    # zenoh-cpp main tip (2026-08-13), in sync with zenoh-c main — required to
    # match the zenoh-c 1.10 line above (the previous March-era pin 481b71bf
    # pairs with zenoh-c 05bd370).
    VCS_URL https://github.com/eclipse-zenoh/zenoh-cpp
    VCS_VERSION 4cbb987cbf5f3329ce69fc7890c36ec7ac0ad95b
    CMAKE_ARGS
      -DZENOHCXX_ZENOHC=OFF
  )

  externalproject_add_stepdependencies(zenoh_cpp_vendor configure zenoh_c_vendor)
endif()

ament_export_dependencies(zenohc)
ament_export_dependencies(zenohcxx)

ament_package()
