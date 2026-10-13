cmake_minimum_required(VERSION 3.16)

project(stbplayer VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(STBP_BUILD_DVB_AUDIO "Build the Linux-DVB hardware audio output" ON)
set(STBP_DVB_AUDIO_VARIANT "normal" CACHE STRING
    "Linux-DVB audio framing variant: normal, dreambox or dags")
set_property(CACHE STBP_DVB_AUDIO_VARIANT PROPERTY STRINGS normal dreambox dags)
option(STBP_DVB_AUDIO_HAVE_EAC3 "Advertise Linux-DVB E-AC3 decoding" ON)
option(STBP_DVB_AUDIO_HAVE_DTS "Advertise Linux-DVB DTS decoding" ON)
option(STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK "PCM AUDIO_GET_PTS counts samples from zero" OFF)
option(STBP_DVB_AUDIO_HISI_VIDEO_CLOCK "Use the HiSi video clock for media-timed DVB audio" OFF)
option(STBP_DVB_AUDIO_HISI_PCM_TRACK "Use a native HiSi slave track for decoded PCM" OFF)
option(STBP_BUILD_HISI_DVB_BACKEND "Build the Linux-DVB backend used by HiSilicon STBs" OFF)
option(STBP_HISI_MV310 "Use the explicit MV310 AVPLAY decoder allocation" OFF)
option(STBP_HISI_LINUX_DVB "Use the receiver Linux-DVB video driver instead of AVPLAY" OFF)
option(STBP_BUILD_BCM_DVB_BACKEND "Build the Linux-DVB/PES backend used by Broadcom STBs" OFF)
option(STBP_BUILD_DREAM_AML_BACKEND "Build the native DreamOne/DreamTwo Amlogic backend" OFF)
set(STBP_BCM_DVB_VARIANT "normal" CACHE STRING
    "Broadcom DVB stream-type/framing variant: normal, type2, dreambox, vuplus or gigablue")
set_property(CACHE STBP_BCM_DVB_VARIANT PROPERTY STRINGS normal type2 dreambox vuplus gigablue)
option(STBP_BCM_DVB_HAVE_HEVC "Advertise Broadcom HEVC decoding" OFF)
option(STBP_BCM_DVB_HAVE_WMV "Advertise Broadcom VC-1/WMV3 decoding" OFF)
option(STBP_BCM_DVB_HAVE_VP6 "Advertise Broadcom VP6 decoding" OFF)
option(STBP_BCM_DVB_HAVE_VP8 "Advertise Broadcom VP8 decoding" OFF)
option(STBP_BCM_DVB_HAVE_VP9 "Advertise Broadcom VP9 decoding" OFF)
option(STBP_BCM_DVB_HAVE_SPARK "Advertise Broadcom Sorenson Spark decoding" OFF)
option(STBP_BCM_DVB_LIMITED_MPEG4V2 "Limit MPEG-4 Part 2 codecs to 800x600" OFF)
option(STBP_BCM_DVB_NEXUS_STC "Use the userspace Nexus host STC API" OFF)
option(STBP_BCM_DVB_STARTUP_CATCHUP "Decode long Broadcom resume preroll in temporary trick mode" OFF)
set(STBP_DEFAULT_BACKEND_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/stbplayer" CACHE PATH
    "Default directory containing STB player backends")

add_library(stbplayer SHARED
  src/loader.c
  src/result.c
)

if(STBP_BUILD_DVB_AUDIO)
  if(NOT STBP_DVB_AUDIO_VARIANT MATCHES "^(normal|dreambox|dags)$")
    message(FATAL_ERROR "Unsupported STBP_DVB_AUDIO_VARIANT: ${STBP_DVB_AUDIO_VARIANT}")
  endif()
  if(STBP_DVB_AUDIO_VARIANT STREQUAL "dreambox")
    set(STBP_DVB_AUDIO_VARIANT_ID 1)
  elseif(STBP_DVB_AUDIO_VARIANT STREQUAL "dags")
    set(STBP_DVB_AUDIO_VARIANT_ID 2)
  else()
    set(STBP_DVB_AUDIO_VARIANT_ID 0)
  endif()
  target_sources(stbplayer PRIVATE
    src/audio_dvb.c
    src/audio_pes.c
  )
  target_compile_definitions(stbplayer PRIVATE
    STBP_DVB_AUDIO_VARIANT=${STBP_DVB_AUDIO_VARIANT_ID}
    STBP_DVB_AUDIO_HAVE_EAC3=$<BOOL:${STBP_DVB_AUDIO_HAVE_EAC3}>
    STBP_DVB_AUDIO_HAVE_DTS=$<BOOL:${STBP_DVB_AUDIO_HAVE_DTS}>
    STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK=$<BOOL:${STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK}>
    STBP_DVB_AUDIO_DTS_SHARED_AC3=$<STREQUAL:${STBP_BCM_DVB_VARIANT},vuplus>
  )
endif()
if(STBP_DVB_AUDIO_HISI_PCM_TRACK)
  if(NOT STBP_BUILD_DVB_AUDIO OR NOT STBP_BUILD_HISI_DVB_BACKEND OR NOT STBP_HISI_LINUX_DVB
     OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "HiSi PCM track requires DVB audio and a 32-bit HiSi Linux-DVB backend")
  endif()
  target_sources(stbplayer PRIVATE src/hisi_pcm.c)
  target_compile_definitions(stbplayer PRIVATE STBP_DVB_AUDIO_HISI_PCM_TRACK=1)
endif()
if(STBP_DVB_AUDIO_HISI_VIDEO_CLOCK)
  if(NOT STBP_BUILD_DVB_AUDIO OR NOT STBP_BUILD_HISI_DVB_BACKEND OR NOT STBP_HISI_LINUX_DVB)
    message(FATAL_ERROR "HiSi audio video clock requires DVB audio and the HiSi Linux-DVB backend")
  endif()
  find_package(Threads REQUIRED)
  target_sources(stbplayer PRIVATE src/hisi_audio_clock.c)
  target_compile_definitions(stbplayer PRIVATE STBP_DVB_AUDIO_HISI_VIDEO_CLOCK=1)
  target_link_libraries(stbplayer PRIVATE Threads::Threads)
endif()
add_library(stbplayer::stbplayer ALIAS stbplayer)

target_compile_features(stbplayer PRIVATE c_std_11)
target_compile_definitions(stbplayer PRIVATE
  STBP_DEFAULT_BACKEND_DIR="${STBP_DEFAULT_BACKEND_DIR}"
)
target_include_directories(stbplayer
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(stbplayer PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(stbplayer PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

configure_file(libstbplayer.pc.in libstbplayer.pc @ONLY)

add_executable(stb-player-probe tools/stb-player-probe.c)
target_compile_features(stb-player-probe PRIVATE c_std_11)
target_link_libraries(stb-player-probe PRIVATE stbplayer::stbplayer)

install(TARGETS stbplayer stb-player-probe
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/stbplayer DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libstbplayer.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

if(STBP_BUILD_HISI_DVB_BACKEND)
  find_package(Threads REQUIRED)
  add_executable(stb-hisi-display tools/stb-hisi-display.c)
  target_compile_features(stb-hisi-display PRIVATE c_std_11)
  target_link_libraries(stb-hisi-display PRIVATE ${CMAKE_DL_LIBS})
  install(TARGETS stb-hisi-display
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  add_library(stbplayer-backend-hisi-dvb MODULE backends/hisi_dvb.c)
  target_compile_features(stbplayer-backend-hisi-dvb PRIVATE c_std_11)
  target_include_directories(stbplayer-backend-hisi-dvb PRIVATE include)
  if(STBP_DVB_AUDIO_HISI_PCM_TRACK)
    target_include_directories(stbplayer-backend-hisi-dvb PRIVATE src)
    target_compile_definitions(stbplayer-backend-hisi-dvb PRIVATE STBP_DVB_AUDIO_HISI_PCM_TRACK=1)
    target_link_libraries(stbplayer-backend-hisi-dvb PRIVATE stbplayer::stbplayer)
  endif()
  if(STBP_DVB_AUDIO_HISI_VIDEO_CLOCK)
    target_include_directories(stbplayer-backend-hisi-dvb PRIVATE src)
    target_compile_definitions(stbplayer-backend-hisi-dvb PRIVATE STBP_DVB_AUDIO_HISI_VIDEO_CLOCK=1)
    target_link_libraries(stbplayer-backend-hisi-dvb PRIVATE stbplayer::stbplayer)
  endif()
  target_compile_definitions(stbplayer-backend-hisi-dvb PRIVATE
    STBP_HISI_MV310=$<BOOL:${STBP_HISI_MV310}>
    STBP_HISI_LINUX_DVB=$<BOOL:${STBP_HISI_LINUX_DVB}>
  )
  # The closed HiSilicon SDK libraries are supplied under different OE recipe
  # names by each receiver brand.  Kodi's HiSi wrapper preloads the common ABI
  # entry libraries before this module is opened, so retain the vendor symbols
  # as runtime-resolved references instead of creating model-specific build
  # dependencies here.
  target_link_libraries(stbplayer-backend-hisi-dvb PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  set_target_properties(stbplayer-backend-hisi-dvb PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "stbplayer-backend-hisi-dvb"
    C_VISIBILITY_PRESET hidden
  )
  install(TARGETS stbplayer-backend-hisi-dvb
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/stbplayer
  )
endif()

if(STBP_BUILD_DREAM_AML_BACKEND)
  find_package(Threads REQUIRED)
  add_library(stbplayer-backend-dream-aml MODULE backends/dream_aml.c)
  target_compile_features(stbplayer-backend-dream-aml PRIVATE c_std_11)
  target_include_directories(stbplayer-backend-dream-aml PRIVATE include)
  target_link_libraries(stbplayer-backend-dream-aml PRIVATE Threads::Threads)
  set_target_properties(stbplayer-backend-dream-aml PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "stbplayer-backend-dream-aml"
    C_VISIBILITY_PRESET hidden
  )
  install(TARGETS stbplayer-backend-dream-aml
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/stbplayer
  )
endif()

if(STBP_BUILD_BCM_DVB_BACKEND)
  if(NOT STBP_BCM_DVB_VARIANT MATCHES "^(normal|type2|dreambox|vuplus|gigablue)$")
    message(FATAL_ERROR "Unsupported STBP_BCM_DVB_VARIANT: ${STBP_BCM_DVB_VARIANT}")
  endif()
  find_package(Threads REQUIRED)
  add_library(stbplayer-backend-bcm-dvb MODULE backends/bcm_dvb.c)
  target_compile_features(stbplayer-backend-bcm-dvb PRIVATE c_std_11)
  target_include_directories(stbplayer-backend-bcm-dvb PRIVATE include)
  string(TOUPPER "${STBP_BCM_DVB_VARIANT}" STBP_BCM_DVB_VARIANT_UPPER)
  target_compile_definitions(stbplayer-backend-bcm-dvb PRIVATE
    STBP_BCM_DVB_VARIANT_${STBP_BCM_DVB_VARIANT_UPPER}=1
    STBP_BCM_DVB_HAVE_HEVC=$<BOOL:${STBP_BCM_DVB_HAVE_HEVC}>
    STBP_BCM_DVB_HAVE_WMV=$<BOOL:${STBP_BCM_DVB_HAVE_WMV}>
    STBP_BCM_DVB_HAVE_VP6=$<BOOL:${STBP_BCM_DVB_HAVE_VP6}>
    STBP_BCM_DVB_HAVE_VP8=$<BOOL:${STBP_BCM_DVB_HAVE_VP8}>
    STBP_BCM_DVB_HAVE_VP9=$<BOOL:${STBP_BCM_DVB_HAVE_VP9}>
    STBP_BCM_DVB_HAVE_SPARK=$<BOOL:${STBP_BCM_DVB_HAVE_SPARK}>
    STBP_BCM_DVB_LIMITED_MPEG4V2=$<BOOL:${STBP_BCM_DVB_LIMITED_MPEG4V2}>
    STBP_BCM_DVB_NEXUS_STC=$<BOOL:${STBP_BCM_DVB_NEXUS_STC}>
    STBP_BCM_DVB_STARTUP_CATCHUP=$<BOOL:${STBP_BCM_DVB_STARTUP_CATCHUP}>
  )
  target_link_libraries(stbplayer-backend-bcm-dvb PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  set_target_properties(stbplayer-backend-bcm-dvb PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "stbplayer-backend-bcm-dvb"
    C_VISIBILITY_PRESET hidden
  )
  install(TARGETS stbplayer-backend-bcm-dvb
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/stbplayer
  )
endif()
