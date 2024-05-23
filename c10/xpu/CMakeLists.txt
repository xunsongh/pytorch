# Build file for the C10 XPU.
#
# C10 XPU is a minimal library, but it does depend on SYCL.

include(../../cmake/public/xpu.cmake)

set(C10_XPU_SRCS
    XPUCachingAllocator.cpp
    XPUFunctions.cpp
    XPUStream.cpp
    impl/XPUGuardImpl.cpp
)
set(C10_XPU_HEADERS
    XPUCachingAllocator.h
    XPUDeviceProp.h
    XPUException.h
    XPUFunctions.h
    XPUMacros.h
    XPUStream.h
    impl/XPUGuardImpl.h
)
if(USE_KINETO)
  find_package(Pti 0.7.0 QUIET)
  if (TARGET Pti::pti_view)
    file(GLOB C10_XPU_PROF_SRCS "profiler/*.cpp")
    file(GLOB C10_XPU_PROF_HEADERS "profiler/*.h")
    list(APPEND C10_XPU_SRCS ${C10_XPU_PROF_SRCS})
    list(APPEND C10_XPU_HEADERS ${C10_XPU_PROF_HEADERS})
    set(C10_XPU_PROF_DEPENDENCY_LIBS kineto fmt::fmt-header-only Pti::pti_view)
  else()
    set(C10_XPU_PROF_DEPENDENCY_LIBS)
  endif()
endif()

add_library(c10_xpu ${C10_XPU_SRCS} ${C10_XPU_HEADERS})
target_compile_options(c10_xpu PRIVATE "-DC10_XPU_BUILD_MAIN_LIB")
# Enable hidden visibility if compiler supports it.
if(${COMPILER_SUPPORTS_HIDDEN_VISIBILITY})
  target_compile_options(c10_xpu PRIVATE "-fvisibility=hidden")
endif()

# ---[ Dependency of c10_xpu
target_link_libraries(c10_xpu PUBLIC c10 torch::xpurt)
target_link_libraries(c10_xpu PRIVATE ${C10_XPU_PROF_DEPENDENCY_LIBS})
target_include_directories(
    c10_xpu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
    )
if(TARGET Pti::pti_view)
  target_include_directories(
      c10_xpu PRIVATE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kineto/libkineto/include>
  )
endif()

add_subdirectory(test)

# ---[ Installation
install(TARGETS c10_xpu EXPORT Caffe2Targets DESTINATION lib)
foreach(file ${C10_XPU_HEADERS})
  get_filename_component(dir ${file} DIRECTORY)
  install(FILES ${file} DESTINATION include/c10/xpu/${dir})
endforeach()
