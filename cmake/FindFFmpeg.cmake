# FFmpeg finder.
# On Linux, FFmpeg comes from the system via pkg-config. On Windows and macOS
# it comes from the obs-deps prebuilt package (found by library/header path).
# Either way this creates imported targets FFmpeg::avcodec, FFmpeg::avutil,
# FFmpeg::swscale and FFmpeg::avformat.

set(_ffmpeg_components avcodec avutil swscale avformat)

if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)

  set(FFmpeg_FOUND TRUE)
  foreach(component IN LISTS _ffmpeg_components)
    pkg_check_modules(PC_${component} IMPORTED_TARGET "lib${component}")
    if(PC_${component}_FOUND)
      if(NOT TARGET FFmpeg::${component})
        add_library(sr_ffmpeg_${component} INTERFACE)
        target_link_libraries(sr_ffmpeg_${component} INTERFACE PkgConfig::PC_${component})
        add_library(FFmpeg::${component} ALIAS sr_ffmpeg_${component})
      endif()
    else()
      set(FFmpeg_FOUND FALSE)
    endif()
  endforeach()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(FFmpeg REQUIRED_VARS FFmpeg_FOUND)
else()
  set(FFmpeg_FOUND TRUE)

  foreach(component IN LISTS _ffmpeg_components)
    find_path(FFMPEG_${component}_INCLUDE_DIR NAMES "lib${component}/${component}.h" PATH_SUFFIXES include)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component} PATH_SUFFIXES lib bin)

    if(FFMPEG_${component}_INCLUDE_DIR AND FFMPEG_${component}_LIBRARY)
      if(NOT TARGET FFmpeg::${component})
        add_library(FFmpeg::${component} UNKNOWN IMPORTED)
        set_target_properties(
          FFmpeg::${component}
          PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_${component}_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${component}_INCLUDE_DIR}"
        )
      endif()
    else()
      set(FFmpeg_FOUND FALSE)
    endif()

    mark_as_advanced(FFMPEG_${component}_INCLUDE_DIR FFMPEG_${component}_LIBRARY)
  endforeach()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(
    FFmpeg
    REQUIRED_VARS FFMPEG_avcodec_LIBRARY FFMPEG_avutil_LIBRARY FFMPEG_swscale_LIBRARY FFMPEG_avformat_LIBRARY
  )
endif()
