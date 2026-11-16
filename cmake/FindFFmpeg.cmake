# FFmpeg finder for the plugin's private, static FFmpeg.
#
# OBS ships FFmpeg as shared libraries whose names carry the major version
# (avcodec-61 up to OBS 32.1, avcodec-62 from 32.2 on), so linking against them
# tied a plugin build to a range of OBS versions. The plugin links its own
# trimmed FFmpeg statically instead (build-aux/ffmpeg/build.sh) and loads in
# every OBS version that shares the libobs ABI.
#
# Looks in SR_FFMPEG_DIR (CMake cache or environment), by default
# .deps-ffmpeg/<platform> in the source tree, and creates imported targets
# FFmpeg::avcodec, FFmpeg::avutil, FFmpeg::swscale and FFmpeg::avformat. The
# system libraries each one needs come from its pkg-config file.

if(WIN32)
  set(_sr_ffmpeg_platform windows-x64)
elseif(APPLE)
  set(_sr_ffmpeg_platform macos-universal)
else()
  set(_sr_ffmpeg_platform linux-${CMAKE_SYSTEM_PROCESSOR})
endif()

if(DEFINED ENV{SR_FFMPEG_DIR} AND NOT "$ENV{SR_FFMPEG_DIR}" STREQUAL "")
  set(_sr_ffmpeg_default "$ENV{SR_FFMPEG_DIR}")
else()
  set(_sr_ffmpeg_default "${CMAKE_SOURCE_DIR}/.deps-ffmpeg/${_sr_ffmpeg_platform}")
endif()
set(SR_FFMPEG_DIR "${_sr_ffmpeg_default}" CACHE PATH "Prefix of the static FFmpeg built by build-aux/ffmpeg")
file(TO_CMAKE_PATH "${SR_FFMPEG_DIR}" _sr_ffmpeg_dir)

# Libraries a component needs besides FFmpeg itself, from its .pc file:
# -lname becomes the library in the prefix if there is one (x264), else the
# system library; -framework pairs are kept for macOS.
function(_sr_ffmpeg_private_libs component out_var)
  set(result "")
  set(pc "${_sr_ffmpeg_dir}/lib/pkgconfig/lib${component}.pc")
  if(EXISTS "${pc}")
    # a static build lists its dependencies in Libs (MSVC) or Libs.private
    file(STRINGS "${pc}" lines REGEX "^Libs(\\.private)?:")
    foreach(line IN LISTS lines)
      string(REGEX REPLACE "^Libs(\\.private)?:[ \t]*" "" line "${line}")
      separate_arguments(tokens UNIX_COMMAND "${line}")
      set(framework_next FALSE)
      foreach(token IN LISTS tokens)
        if(framework_next)
          list(APPEND result "-framework ${token}")
          set(framework_next FALSE)
        elseif(token STREQUAL "-framework")
          set(framework_next TRUE)
        elseif(token MATCHES "^-(L|libpath:)")
          # search paths: libraries are resolved against the prefix below
        elseif(token MATCHES "^-l(.+)$")
          set(name "${CMAKE_MATCH_1}")
          if(name MATCHES "^(avcodec|avformat|avutil|swscale)$")
            continue()
          endif()
          unset(found CACHE)
          find_library(found NAMES ${name} lib${name} PATHS "${_sr_ffmpeg_dir}/lib" NO_DEFAULT_PATH NO_CACHE)
          if(found)
            list(APPEND result "${found}")
          else()
            list(APPEND result "${name}")
          endif()
        elseif(token MATCHES "^-pthread$")
          list(APPEND result Threads::Threads)
        elseif(token MATCHES "^([^/:]+)\\.lib$")
          # MSVC style: a library built here (libx264.lib) or a system one
          if(EXISTS "${_sr_ffmpeg_dir}/lib/${token}")
            list(APPEND result "${_sr_ffmpeg_dir}/lib/${token}")
          else()
            list(APPEND result "${token}")
          endif()
        endif()
      endforeach()
    endforeach()
  endif()
  list(REMOVE_DUPLICATES result)
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

set(FFmpeg_FOUND TRUE)
set(_sr_ffmpeg_components avutil swscale avcodec avformat)
set(_sr_ffmpeg_deps_avutil "")
set(_sr_ffmpeg_deps_swscale FFmpeg::avutil)
set(_sr_ffmpeg_deps_avcodec FFmpeg::avutil)
set(_sr_ffmpeg_deps_avformat FFmpeg::avcodec FFmpeg::avutil)

foreach(component IN LISTS _sr_ffmpeg_components)
  find_library(
    SR_FFMPEG_${component}_LIBRARY
    NAMES lib${component}.a ${component}
    PATHS "${_sr_ffmpeg_dir}/lib"
    NO_DEFAULT_PATH
  )
  mark_as_advanced(SR_FFMPEG_${component}_LIBRARY)

  if(NOT SR_FFMPEG_${component}_LIBRARY OR NOT EXISTS "${_sr_ffmpeg_dir}/include/lib${component}/${component}.h")
    set(FFmpeg_FOUND FALSE)
    continue()
  endif()

  if(NOT TARGET FFmpeg::${component})
    _sr_ffmpeg_private_libs(${component} _private)
    add_library(FFmpeg::${component} STATIC IMPORTED)
    set_target_properties(
      FFmpeg::${component}
      PROPERTIES
        IMPORTED_LOCATION "${SR_FFMPEG_${component}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${_sr_ffmpeg_dir}/include"
        INTERFACE_LINK_LIBRARIES "${_sr_ffmpeg_deps_${component}};${_private}"
    )
  endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  FFmpeg
  REQUIRED_VARS SR_FFMPEG_avcodec_LIBRARY SR_FFMPEG_avutil_LIBRARY SR_FFMPEG_swscale_LIBRARY SR_FFMPEG_avformat_LIBRARY
  REASON_FAILURE_MESSAGE "build it with build-aux/ffmpeg/build.sh and set SR_FFMPEG_DIR (looked in ${_sr_ffmpeg_dir})"
)
