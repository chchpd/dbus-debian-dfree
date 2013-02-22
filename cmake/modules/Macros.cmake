
MACRO(TIMESTAMP RESULT)
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        EXECUTE_PROCESS(COMMAND "cmd" " /C date /T" OUTPUT_VARIABLE DATE)
        string(REGEX REPLACE "(..)[/.](..)[/.](....).*" "\\3\\2\\1" DATE ${DATE})
        EXECUTE_PROCESS(COMMAND "cmd" " /C time /T" OUTPUT_VARIABLE TIME)
        string(REGEX REPLACE "(..):(..)" "\\1\\2" TIME ${TIME})
        set (${RESULT} "${DATE}${TIME}")
    else ()
        EXECUTE_PROCESS(COMMAND "date" "+%Y%m%d%H%M" OUTPUT_VARIABLE ${RESULT})
    endif ()
ENDMACRO()
