# Deploy the framework INI as the template's layout and comments with the user's values.
#
# The user edits the live INI (bDebug, bRagdollTrace, ...), so it is never replaced by the
# template. Copying it only when absent left installed files without new keys and with retired
# ones (code review 2026-09-23 P-2). deploy_ini.ps1 merges instead: every value the user set is
# kept, new keys appear with their defaults, retired keys are dropped (reported). No backup file
# is left in the plugin folder.
execute_process(
    COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_LIST_DIR}/deploy_ini.ps1" -Template "${SRC}" -Target "${DST}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(output)
    message(STATUS "${output}")
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "INI deploy failed (${result}): ${error}")
endif()
