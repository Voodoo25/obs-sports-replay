# Minimal FFmpeg finder for the obs-deps prebuilt package.
# Creates imported targets FFmpeg::avcodec, FFmpeg::avutil, FFmpeg::swscale,
# FFmpeg::avformat when found in CMAKE_PREFIX_PATH.

set(_ffmpeg_components avcodec avutil swscale avformat)

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
