# Writes OUT (a header with MDR_COMMIT, the checkout's commit, "+" when it has
# changes) at every build, touching it only when the text changes. Run with
# cmake -DSRC=<source dir> -DOUT=<header> -P commit.cmake.
execute_process(COMMAND git -C "${SRC}" rev-parse --short HEAD
                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no -- src tools CMakeLists.txt
                OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT commit)
    set(commit "unknown")
endif()
if(dirty)
    set(commit "${commit}+")
endif()
set(text "#define MDR_COMMIT \"${commit}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
