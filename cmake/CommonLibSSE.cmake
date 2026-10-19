# CommonLibSSE-NG for builds without a shared install: the alandtse line, pinned to a reviewed commit.
#
# Why this line: it classifies runtime minor >= 6 as AE and reads the Address Library format from the file,
# so Skyrim 1.7.x (Address Library FORMAT 5) resolves its ids. Never point GIT_TAG at a branch: a moving
# branch puts unreviewed code into the next build.
if(TARGET CommonLibSSE::CommonLibSSE)
    return()
endif()

set(COMMONLIB_GIT_REPOSITORY "https://github.com/alandtse/CommonLibVR.git")
set(COMMONLIB_GIT_TAG "0e167dd4c3981d586a53953e2c1a608e2c4ccedf")  # ng @ 2026-08-21

# CommonLibSSE's own tests and tooling stay off (Catch2 and regex conflicts).
option(CommonLibSSE_POSH_GENERATION "Generate .posh.d.ts type definition file" OFF)
option(CommonLibSSE_TESTING "Build tests" OFF)
set(CommonLibSSE_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

message(STATUS "CommonLibSSE: fetching ${COMMONLIB_GIT_REPOSITORY} at ${COMMONLIB_GIT_TAG}")
include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY ${COMMONLIB_GIT_REPOSITORY}
    GIT_TAG ${COMMONLIB_GIT_TAG}
)
FetchContent_MakeAvailable(CommonLibSSE)
