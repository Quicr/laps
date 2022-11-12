
file(GLOB MEDIA_SOURCES *.cxx)
file(GLOB MEDIA_HEADER *.h)

add_library( laps  ${MEDIA_SOURCES} ${MEDIA_HEADERS} )

