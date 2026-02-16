# Mandatory Dependencies
pkg_check_modules(LIBYAML REQUIRED yaml-0.1)

# Optional Modules
option(BUILD_TESTS "Testing" OFF)
option(BUILD_STATE_DETECTOR "Display Detector" OFF)

# Restune Core Lib
file(GLOB SOURCES "core/*.cpp"
                  "core/Server/*.cpp"
                  "signals/*.cpp"
                  "init/*.cpp")

set(LINK_LIBS "")
list(APPEND LINK_LIBS
  UrmAuxUtils
  UrmExtAPIs
  ${LIBYAML_LIBRARIES}
)

pkg_check_modules(PC_SYSTEMD QUIET systemd)
if(PC_SYSTEMD_FOUND)
  list(APPEND LINK_LIBS libsystemd)
  list(APPEND SOURCES dbus-modules/RestuneDBusInternal.cpp)
else()
  list(APPEND SOURCES dbus-modules/RestuneDBusStubs.cpp)
endif()

if(BUILD_STATE_DETECTOR)
  list(APPEND SOURCES dbus-modules/StateDetector.cpp)
endif()

add_library(RestuneCore ${SOURCES})
set_target_properties(RestuneCore PROPERTIES VERSION 1.0.0 SOVERSION 1)
target_include_directories(RestuneCore PRIVATE ${LIBYAML_INCLUDE_DIRS})
target_link_libraries(RestuneCore PUBLIC ${LINK_LIBS})

target_include_directories(RestuneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/Include)
target_include_directories(RestuneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/Server/Include)
target_include_directories(RestuneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/signals/Include)
target_include_directories(RestuneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/init/Include)
target_include_directories(RestuneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/dbus-modules/Include)
install(TARGETS RestuneCore LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
