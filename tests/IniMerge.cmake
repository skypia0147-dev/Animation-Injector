# ctest IniMerge: cmake/deploy_ini.ps1 on an old installed INI (P-2).
# Inputs: TEMPLATE (misc/AnimationInjector.ini), OLD (an installed file with three retired keys and
# without nine new keys), EXPECTED (the merge: every old value kept), WORK (a scratch directory).
set(script "${CMAKE_CURRENT_LIST_DIR}/../cmake/deploy_ini.ps1")
file(MAKE_DIRECTORY "${WORK}")
function(deploy target out)
    execute_process(
        COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${script}" -Template "${TEMPLATE}" -Target "${target}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "deploy_ini.ps1 failed (${result}): ${error}")
    endif()
    set(${out} "${output}" PARENT_SCOPE)
endfunction()
function(same a b what)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${a}" "${b}" RESULT_VARIABLE differ)
    if(differ)
        message(FATAL_ERROR "${what}: ${a} differs from ${b}")
    endif()
endfunction()

# An old installed file: merged into the template with every value kept, no backup left beside it.
file(REMOVE "${WORK}/AnimationInjector.ini" "${WORK}/AnimationInjector.ini.previous")
file(COPY_FILE "${OLD}" "${WORK}/AnimationInjector.ini")
deploy("${WORK}/AnimationInjector.ini" first)
if(NOT first MATCHES "added: 9; dropped: 4" OR NOT first MATCHES "dropped \\[Inject\\] fStopTriggerBeforeEnd")
    message(FATAL_ERROR "unexpected merge report: ${first}")
endif()
same("${WORK}/AnimationInjector.ini" "${EXPECTED}" "merge")
if(EXISTS "${WORK}/AnimationInjector.ini.previous")
    message(FATAL_ERROR "the merge left a backup file")
endif()

# A second deploy changes nothing.
deploy("${WORK}/AnimationInjector.ini" second)
if(NOT second MATCHES "INI up to date")
    message(FATAL_ERROR "second deploy rewrote the file: ${second}")
endif()
same("${WORK}/AnimationInjector.ini" "${EXPECTED}" "second deploy")

# A first install is the template itself.
file(REMOVE "${WORK}/fresh.ini")
deploy("${WORK}/fresh.ini" fresh)
same("${WORK}/fresh.ini" "${TEMPLATE}" "first install")
message(STATUS "INI merge keeps user values, adds new keys, drops retired ones")
