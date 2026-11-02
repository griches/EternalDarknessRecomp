# Writes OUT (ed_build_id.h): the commit a build is from, for problem reports
# (runtime/host/report.c). Runs at every build and rewrites the file only when
# the commit (or whether there are uncommitted changes) differs.
# A copy of the tree (the Deck's, the PC's) has .build-commit beside it,
# written by whoever copied it: the commit it was copied from, which wins over
# whatever git the copy may hold.
if(EXISTS "${SRC}/.build-commit")
    file(STRINGS "${SRC}/.build-commit" commit LIMIT_COUNT 1)
else()
    execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD
                    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT commit)
        set(commit "unknown")
    else()
        execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
                        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(dirty)
            set(commit "${commit} with uncommitted changes")
        endif()
    endif()
endif()
# The version the menus show: the release tag, and the commit when the build
# is past it ("0.9.6 + 7cfb51d"); a copy without git shows its commit.
execute_process(COMMAND git -C "${SRC}" describe --tags --abbrev=7 --match "v[0-9]*"
                OUTPUT_VARIABLE described OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(EXISTS "${SRC}/.build-commit" OR NOT described)
    string(REGEX REPLACE " .*" "" version "${commit}")
elseif(described MATCHES "^v([^-]+)-[0-9]+-g([0-9a-f]+)$")
    set(version "${CMAKE_MATCH_1} + ${CMAKE_MATCH_2}")
else()
    string(REGEX REPLACE "^v" "" version "${described}")
endif()
set(text "#define ED_BUILD_COMMIT \"${commit}\"\n#define ED_BUILD_VERSION \"${version}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
