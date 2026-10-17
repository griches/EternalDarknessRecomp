# Writes OUT (ed_build_id.h): the commit a build is from, for problem reports
# (runtime/host/report.c). Runs at every build and rewrites the file only when
# the commit (or whether there are uncommitted changes) differs.
execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD
                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT commit AND EXISTS "${SRC}/.build-commit")
    # A copy of the tree without .git (the Deck's and the PC's): the commit the
    # copy was made from, written beside it by whoever copied it.
    file(STRINGS "${SRC}/.build-commit" commit LIMIT_COUNT 1)
elseif(NOT commit)
    set(commit "unknown")
else()
    execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(dirty)
        set(commit "${commit} with uncommitted changes")
    endif()
endif()
set(text "#define ED_BUILD_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
