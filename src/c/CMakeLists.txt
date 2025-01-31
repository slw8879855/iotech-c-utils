# Set additional C compiler flags

if (DEFINED CMAKE_C_COMPILER_ID)
  if (CMAKE_C_COMPILER_ID MATCHES GNU)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wstrict-prototypes -Wmissing-prototypes")
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread -pipe")
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")
    if (IOT_BUILD_LCOV)
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
      set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")
    endif ()
  endif ()
endif ()
if ("${LINUX_SYS}" STREQUAL "Alpine")
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__LIBMUSL__")
endif ()

# Set files to compile

set (C_FILES iot.c data.c json.c base64.c logger.c scheduler.c thread.c threadpool.c time.c component.c hash.c)
if (IOT_BUILD_COMPONENTS)
  set (C_FILES ${C_FILES} container.c)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DIOT_BUILD_COMPONENTS")
endif ()

# Main library

add_library (iot SHARED ${C_FILES})
add_library (iot-static STATIC ${C_FILES})
set_target_properties (iot PROPERTIES OUTPUT_NAME iot CLEAN_DIRECT_OUTPUT 1)
set_target_properties (iot-static PROPERTIES OUTPUT_NAME iot CLEAN_DIRECT_OUTPUT 1)
target_include_directories (iot PRIVATE ../../include)
target_include_directories (iot-static PRIVATE ../../include)

# Build modules

add_subdirectory (cunit)
add_subdirectory (examples)
add_subdirectory (utests)
 
# Configure installer

install (TARGETS iot LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install (TARGETS iot-static ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install (DIRECTORY "${CMAKE_SOURCE_DIR}/../include/" DESTINATION include)
