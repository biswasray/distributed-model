# Copies SRC to DST only if DST does not exist yet, so a file the user edited is never overwritten.
#   cmake -DSRC=<file> -DDST=<file> -P CopyIfMissing.cmake
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Created ${DST} from ${SRC}")
endif()
