cmake_minimum_required(VERSION 3.20)
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "" FORCE)
project(AnimationInjector VERSION 0.12.279)
enable_language(RC)
add_executable(AnimationInjectorFileLifetimeTests EXCLUDE_FROM_ALL tests/AnimationFileLifetime.cpp)
target_include_directories(AnimationInjectorFileLifetimeTests PRIVATE tests/file_lifetime_stubs src)
target_compile_features(AnimationInjectorFileLifetimeTests PRIVATE cxx_std_20)
# Sparse per-actor runners (runtime/RunnerSet.h): ascending walks and lock-free reads while creating.
add_executable(AnimationInjectorRunnerSetTests EXCLUDE_FROM_ALL tests/RunnerSet.cpp)
target_include_directories(AnimationInjectorRunnerSetTests PRIVATE src)
target_compile_features(AnimationInjectorRunnerSetTests PRIVATE cxx_std_20)
# The followed root CarryPoseLock hands to the animation thread (core/SeqLock.h): no torn reads.
add_executable(AnimationInjectorSeqLockTests EXCLUDE_FROM_ALL tests/SeqLock.cpp)
target_include_directories(AnimationInjectorSeqLockTests PRIVATE src)
target_compile_features(AnimationInjectorSeqLockTests PRIVATE cxx_std_20)
# One ledger for weapon hides shared by pair effects and carries (runtime/WeaponCull.h).
add_executable(AnimationInjectorWeaponCullTests EXCLUDE_FROM_ALL tests/WeaponCull.cpp)
target_include_directories(AnimationInjectorWeaponCullTests PRIVATE src)
target_compile_features(AnimationInjectorWeaponCullTests PRIVATE cxx_std_20)
# Our entries in another plugin's opt-out list (runtime/ExternalHolds.h, used by PrecisionLink).
add_executable(AnimationInjectorExternalHoldsTests EXCLUDE_FROM_ALL tests/ExternalHolds.cpp)
target_include_directories(AnimationInjectorExternalHoldsTests PRIVATE src)
target_compile_features(AnimationInjectorExternalHoldsTests PRIVATE cxx_std_20)
add_executable(AnimationInjectorPairedClipIndexTests EXCLUDE_FROM_ALL tests/PairedClipIndex.cpp)
target_include_directories(AnimationInjectorPairedClipIndexTests PRIVATE src)
target_compile_features(AnimationInjectorPairedClipIndexTests PRIVATE cxx_std_20)
configure_file(cmake/Version.h.in generated/Version.h @ONLY)
configure_file(cmake/version.rc.in generated/version.rc @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(AnimationInjectorContinuityTests EXCLUDE_FROM_ALL tests/CarryContinuity.cpp)
add_executable(AnimationInjectorPartialRagdollTests EXCLUDE_FROM_ALL tests/PartialRagdoll.cpp)
target_include_directories(AnimationInjectorPartialRagdollTests PRIVATE src)
target_include_directories(AnimationInjectorContinuityTests PRIVATE src)
add_executable(AnimationInjectorAlignmentTests EXCLUDE_FROM_ALL tests/PairAlignment.cpp)
add_executable(AnimationInjectorNativeMarkTests EXCLUDE_FROM_ALL tests/NativePairMarkCapture.cpp)
target_include_directories(AnimationInjectorNativeMarkTests PRIVATE src)
target_include_directories(AnimationInjectorAlignmentTests PRIVATE src)
add_executable(AnimationInjectorStartControlTests EXCLUDE_FROM_ALL tests/PairStartControl.cpp src/runtime/PairStartControl.cpp)
target_include_directories(AnimationInjectorStartControlTests PRIVATE tests/start_control_stubs src)
add_executable(AnimationInjectorStartCameraTests EXCLUDE_FROM_ALL tests/PairStartCamera.cpp src/runtime/PairStartCamera.cpp)
target_include_directories(AnimationInjectorStartCameraTests PRIVATE tests/start_camera_stubs src)

# Standalone checks use no game runtime or additional test dependency.
add_executable(AnimationInjectorCarryTests EXCLUDE_FROM_ALL tests/CarryOwnership.cpp tests/CarryPlacement.cpp tests/CarrySyncResolver.cpp src/core/CarrySyncResolver.cpp)
target_include_directories(AnimationInjectorCarryTests PRIVATE src)

add_executable(AnimationInjectorMathTests EXCLUDE_FROM_ALL tests/PairMotionMath.cpp tests/PairedCleanup.cpp)
target_include_directories(AnimationInjectorMathTests PRIVATE src)

add_executable(AnimationInjectorPairRoleTests EXCLUDE_FROM_ALL tests/PairEventRoles.cpp)
target_include_directories(AnimationInjectorPairRoleTests PRIVATE src)
target_compile_features(AnimationInjectorPairRoleTests PRIVATE cxx_std_20)
add_executable(AnimationInjectorCameraTests EXCLUDE_FROM_ALL tests/CameraRecovery.cpp)
target_include_directories(AnimationInjectorCameraTests PRIVATE src)

add_executable(AnimationInjectorDrivenTests EXCLUDE_FROM_ALL tests/DrivenCleanup.cpp)
add_executable(AnimationInjectorExitBlendTests EXCLUDE_FROM_ALL tests/ExitBlendMovement.cpp)
target_include_directories(AnimationInjectorExitBlendTests PRIVATE src)
target_include_directories(AnimationInjectorDrivenTests PRIVATE src)

add_executable(AnimationInjectorPreloadTests EXCLUDE_FROM_ALL tests/AnimationPreloader.cpp src/core/LoadingProgress.cpp)
target_include_directories(AnimationInjectorPreloadTests PRIVATE tests/preload_stubs src)
add_executable(AnimationInjectorLoadingTests EXCLUDE_FROM_ALL tests/LoadingProgress.cpp)
target_include_directories(AnimationInjectorLoadingTests PRIVATE src)
add_executable(AnimationInjectorLoadObserverTests EXCLUDE_FROM_ALL tests/AnimationLoadObserver.cpp src/core/LoadingProgress.cpp)
target_include_directories(AnimationInjectorLoadObserverTests PRIVATE tests/load_observer_stubs src)

add_executable(AnimationInjectorTemplateCloneTests EXCLUDE_FROM_ALL tests/TemplateNodeClone.cpp)
target_include_directories(AnimationInjectorTemplateCloneTests PRIVATE src)
add_executable(AnimationInjectorPairedRigTests EXCLUDE_FROM_ALL tests/PairedRigCatalog.cpp)
target_include_directories(AnimationInjectorPairedRigTests PRIVATE src)

# Compile the real link state machine against deterministic actor/engine doubles.
add_executable(AnimationInjectorLinkTests EXCLUDE_FROM_ALL tests/ActionLinks.cpp src/runtime/ActionLinks.cpp)
target_include_directories(AnimationInjectorLinkTests PRIVATE tests/link_stubs src)

option(CommonLibSSE_POSH_GENERATION "Generate .posh.d.ts type definition file" OFF)
option(CommonLibSSE_TESTING "Build tests" OFF)
set(CommonLibSSE_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK    ON  CACHE BOOL "" FORCE)

find_package(fmt    CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(xbyak  CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)   # XML declaration parser

include(FetchContent)
# --- CommonLibSSE-NG ---
# The developer's machine names a shared prebuilt install in cmake/Local.cmake (not published); everywhere else
# cmake/CommonLibSSE.cmake fetches the same pinned commit and builds it from source.
include("${CMAKE_SOURCE_DIR}/cmake/Local.cmake" OPTIONAL)
if(AI_COMMONLIB_ENTRY AND EXISTS "${AI_COMMONLIB_ENTRY}")
    include("${AI_COMMONLIB_ENTRY}")
else()
    include("${CMAKE_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
endif()

# Exercise placement with the same NiTransform operations used by the DLL.
target_link_libraries(AnimationInjectorCarryTests PRIVATE CommonLibSSE::CommonLibSSE)
add_executable(AnimationInjectorLimbFrameTests EXCLUDE_FROM_ALL tests/CarryLimbFrame.cpp)
target_include_directories(AnimationInjectorLimbFrameTests PRIVATE src)
target_link_libraries(AnimationInjectorLimbFrameTests PRIVATE CommonLibSSE::CommonLibSSE)
# Joint angles against Havok limits: golden values from HKX_Edit's calculator.
add_executable(AnimationInjectorJointAnglesTests EXCLUDE_FROM_ALL tests/CarryJointAngles.cpp)
target_include_directories(AnimationInjectorJointAnglesTests PRIVATE src)
target_link_libraries(AnimationInjectorJointAnglesTests PRIVATE CommonLibSSE::CommonLibSSE)
add_executable(AnimationInjectorCarryHoldTests EXCLUDE_FROM_ALL tests/CarryHold.cpp)
target_include_directories(AnimationInjectorCarryHoldTests PRIVATE src)
target_link_libraries(AnimationInjectorCarryHoldTests PRIVATE CommonLibSSE::CommonLibSSE)
add_executable(AnimationInjectorStartBlendTests EXCLUDE_FROM_ALL tests/PairStartBlend.cpp)
target_include_directories(AnimationInjectorStartBlendTests PRIVATE src)
target_link_libraries(AnimationInjectorStartBlendTests PRIVATE CommonLibSSE::CommonLibSSE)

# Ragdoll calibration trace (P5): bounded recording and the file HKX_Edit reads back.
add_executable(AnimationInjectorRagdollTraceTests EXCLUDE_FROM_ALL tests/RagdollTrace.cpp src/core/RagdollTrace.cpp)
target_include_directories(AnimationInjectorRagdollTraceTests PRIVATE src)
target_compile_definitions(AnimationInjectorRagdollTraceTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorRagdollTraceTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorRagdollTraceTests PRIVATE CommonLibSSE::CommonLibSSE)
if(MSVC)
    target_compile_options(AnimationInjectorRagdollTraceTests PRIVATE /utf-8)
endif()

# Offline CLI and semantic comparisons compile the same parser used by the DLL.
add_executable(AnimationInjectorXmlCheck EXCLUDE_FROM_ALL tests/XmlDeclarations.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp src/runtime/RagdollProfile.cpp)
add_executable(AnimationInjectorQteTests EXCLUDE_FROM_ALL tests/Qte.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorQteTests PRIVATE src)
target_compile_definitions(AnimationInjectorQteTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AnimationInjectorQteTests PRIVATE /utf-8)
target_precompile_headers(AnimationInjectorQteTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorQteTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
add_executable(AnimationInjectorQteLifecycleTests EXCLUDE_FROM_ALL tests/QteLifecycle.cpp src/runtime/Qte.cpp)
# How a QTE shows its key (0.12.245): the circle's middle, a one-press QTE's key alone and its press.
add_executable(AnimationInjectorQteFaceTests EXCLUDE_FROM_ALL tests/QteFace.cpp)
target_include_directories(AnimationInjectorQteFaceTests PRIVATE src)
target_compile_features(AnimationInjectorQteFaceTests PRIVATE cxx_std_20)
# The converging-lines look (0.12.246): timing, result looks, screen effects, particles.
add_executable(AnimationInjectorQteConvergeTests EXCLUDE_FROM_ALL tests/QteConverge.cpp)
target_include_directories(AnimationInjectorQteConvergeTests PRIVATE src)
target_compile_features(AnimationInjectorQteConvergeTests PRIVATE cxx_std_20)
add_executable(AnimationInjectorQteLayoutTests EXCLUDE_FROM_ALL tests/QteLayout.cpp)
target_include_directories(AnimationInjectorQteLayoutTests PRIVATE src)
target_compile_features(AnimationInjectorQteLayoutTests PRIVATE cxx_std_20)
target_include_directories(AnimationInjectorQteLifecycleTests PRIVATE tests/qte_stubs src)
target_compile_features(AnimationInjectorQteLifecycleTests PRIVATE cxx_std_20)
target_compile_options(AnimationInjectorQteLifecycleTests PRIVATE /utf-8)
add_executable(AnimationInjectorRuntimeExtensionTests EXCLUDE_FROM_ALL tests/RuntimeExtensions.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorRuntimeExtensionTests PRIVATE src)
target_compile_definitions(AnimationInjectorRuntimeExtensionTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorRuntimeExtensionTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorRuntimeExtensionTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
if(MSVC)
    target_compile_options(AnimationInjectorRuntimeExtensionTests PRIVATE /utf-8)
endif()
# The global event registry (XML format 3 on): grammar, rejected old forms and the merge of every file.
add_executable(AnimationInjectorEventRegistryTests EXCLUDE_FROM_ALL tests/EventRegistry.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorEventRegistryTests PRIVATE src)
target_compile_definitions(AnimationInjectorEventRegistryTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AnimationInjectorEventRegistryTests PRIVATE /utf-8)
target_precompile_headers(AnimationInjectorEventRegistryTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorEventRegistryTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
# XML format 4: keys in place, pad names, the omitted Window, unified defaults, removed forms named.
add_executable(AnimationInjectorFormat4Tests EXCLUDE_FROM_ALL tests/Format4.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorFormat4Tests PRIVATE src)
target_compile_definitions(AnimationInjectorFormat4Tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AnimationInjectorFormat4Tests PRIVATE /utf-8)
target_precompile_headers(AnimationInjectorFormat4Tests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorFormat4Tests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
# Declaration folders named outside the ANSI code page load without throwing (code review 2026-09-30 R-14).
add_executable(AnimationInjectorDeclarationPathsTests EXCLUDE_FROM_ALL tests/DeclarationPaths.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp src/runtime/RagdollProfile.cpp)
target_include_directories(AnimationInjectorDeclarationPathsTests PRIVATE src)
target_compile_definitions(AnimationInjectorDeclarationPathsTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AnimationInjectorDeclarationPathsTests PRIVATE /utf-8)
target_precompile_headers(AnimationInjectorDeclarationPathsTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorDeclarationPathsTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
# Camera views (runtime/ViewSpec.h): the rules ViewGuard applies and the view / firstPerson grammar.
add_executable(AnimationInjectorViewTests EXCLUDE_FROM_ALL tests/View.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorViewTests PRIVATE src)
target_compile_definitions(AnimationInjectorViewTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AnimationInjectorViewTests PRIVATE /utf-8)
target_precompile_headers(AnimationInjectorViewTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorViewTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
target_include_directories(AnimationInjectorXmlCheck PRIVATE src)
target_compile_definitions(AnimationInjectorXmlCheck PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorXmlCheck PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorXmlCheck PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
if(MSVC)
    target_compile_options(AnimationInjectorXmlCheck PRIVATE /utf-8)
endif()

# Scale bench (not a ctest): the declaration pipeline at load over a stress folder
# (tools/scale/generate_stress.py). Built on demand: --target AnimationInjectorScaleBench.
add_executable(AnimationInjectorScaleBench EXCLUDE_FROM_ALL tests/ScaleBench.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp src/runtime/RagdollProfile.cpp)
target_include_directories(AnimationInjectorScaleBench PRIVATE src)
target_compile_definitions(AnimationInjectorScaleBench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorScaleBench PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorScaleBench PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
if(MSVC)
    target_compile_options(AnimationInjectorScaleBench PRIVATE /utf-8)
endif()

# Stage 3 (R-6): clip index equivalence with the former linear lookup, plus its cost.
add_executable(AnimationInjectorClipIndexTests EXCLUDE_FROM_ALL tests/ClipIndex.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorClipIndexTests PRIVATE src)
target_compile_definitions(AnimationInjectorClipIndexTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorClipIndexTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorClipIndexTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
if(MSVC)
    target_compile_options(AnimationInjectorClipIndexTests PRIVATE /utf-8)
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")
add_executable(AnimationInjectorConditionFlowTests EXCLUDE_FROM_ALL tests/ConditionFlow.cpp)
add_executable(AnimationInjectorExtendedConditionTests EXCLUDE_FROM_ALL tests/ExtendedConditions.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorExtendedConditionTests PRIVATE src)
target_link_libraries(AnimationInjectorExtendedConditionTests PRIVATE pugixml::pugixml)
target_compile_features(AnimationInjectorExtendedConditionTests PRIVATE cxx_std_20)
add_executable(AnimationInjectorConditionFlowXmlTests EXCLUDE_FROM_ALL tests/ConditionFlowXml.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorConditionFlowXmlTests PRIVATE src)
add_executable(AnimationInjectorGrappleFlowTests EXCLUDE_FROM_ALL tests/GrappleFlow.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorGrappleFlowTests PRIVATE src)
target_link_libraries(AnimationInjectorGrappleFlowTests PRIVATE pugixml::pugixml)
target_compile_features(AnimationInjectorGrappleFlowTests PRIVATE cxx_std_20)
target_link_libraries(AnimationInjectorConditionFlowXmlTests PRIVATE pugixml::pugixml)
target_compile_features(AnimationInjectorConditionFlowXmlTests PRIVATE cxx_std_20)
target_include_directories(AnimationInjectorConditionFlowTests PRIVATE src)
target_compile_features(AnimationInjectorConditionFlowTests PRIVATE cxx_std_20)
add_library(${PROJECT_NAME} MODULE ${SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
target_include_directories(${PROJECT_NAME} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Reuse the decoder already compiled into our pinned CommonLib dependency (hde64, which CommonLib vendors).
# A shared install does not populate it, so its entry point names the shared copy; a source build uses the one
# FetchContent downloaded.
if(COMMONLIB_HDE64_SOURCE_DIR)
    set(AI_HDE64_SOURCE_DIR "${COMMONLIB_HDE64_SOURCE_DIR}")
else()
    FetchContent_GetProperties(hde64 SOURCE_DIR AI_HDE64_SOURCE_DIR)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${AI_HDE64_SOURCE_DIR}/src/hde")
target_include_directories(AnimationInjectorCarryTests PRIVATE "${AI_HDE64_SOURCE_DIR}/src/hde")
add_executable(AnimationInjectorSpaceDecoderTests EXCLUDE_FROM_ALL tests/SpaceDecoder.cpp)
target_include_directories(AnimationInjectorSpaceDecoderTests PRIVATE "${AI_HDE64_SOURCE_DIR}/src/hde")
target_link_libraries(AnimationInjectorSpaceDecoderTests PRIVATE CommonLibSSE::CommonLibSSE)
add_executable(AnimationInjectorPairInputTests EXCLUDE_FROM_ALL tests/PairInputGate.cpp src/core/PairInputGateResolver.cpp)
target_include_directories(AnimationInjectorPairInputTests PRIVATE src "${AI_HDE64_SOURCE_DIR}/src/hde")
target_link_libraries(AnimationInjectorPairInputTests PRIVATE CommonLibSSE::CommonLibSSE)
add_executable(AnimationInjectorPairMotionTests EXCLUDE_FROM_ALL tests/PairMotionGate.cpp src/core/PairInputGateResolver.cpp)
target_include_directories(AnimationInjectorPairMotionTests PRIVATE src "${AI_HDE64_SOURCE_DIR}/src/hde")
target_link_libraries(AnimationInjectorPairMotionTests PRIVATE CommonLibSSE::CommonLibSSE)
add_executable(AnimationInjectorRecoveryLocomotionTests EXCLUDE_FROM_ALL tests/RecoveryLocomotion.cpp)
target_include_directories(AnimationInjectorRecoveryLocomotionTests PRIVATE src)
target_compile_features(AnimationInjectorRecoveryLocomotionTests PRIVATE cxx_std_20)
target_compile_definitions(${PROJECT_NAME} PRIVATE SKSE_SUPPORT_XBYAK UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE xbyak::xbyak pugixml::pugixml)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "AnimationInjector" PREFIX "" SUFFIX ".dll")

# Keep optimized builds symbolized so runtime crash reports can resolve our frames.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/Zi>")
    target_link_options(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Release>:/DEBUG:FULL>"
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
        "$<$<CONFIG:Release>:/INCREMENTAL:NO>")
endif()

# Deployment is opt-in for source consumers. The INI keeps every user value (cmake/deploy_ini.ps1).
set(AI_DEPLOY_DIR "" CACHE PATH "Optional SKSE/Plugins deployment directory")
if(AI_DEPLOY_DIR)
set(DEPLOY_DIR "${AI_DEPLOY_DIR}")
get_filename_component(AI_DATA_DIR "${DEPLOY_DIR}/../.." ABSOLUTE)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DEPLOY_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AI_DATA_DIR}/Interface"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/Interface/AnimationInjectorLoading.swf" "${AI_DATA_DIR}/Interface/AnimationInjectorLoading.swf"
    COMMAND ${CMAKE_COMMAND} -D "SRC=${CMAKE_SOURCE_DIR}/misc/AnimationInjector.ini" -D "DST=${DEPLOY_DIR}/AnimationInjector.ini" -P "${CMAKE_SOURCE_DIR}/cmake/deploy_ini.cmake"
    COMMENT "Deploying AnimationInjector.dll (+INI merged with user values) to ${AI_DEPLOY_DIR}")

if(MSVC)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DEPLOY_DIR}/AnimationInjector.pdb"
        COMMENT "Deploying matching AnimationInjector.pdb")
endif()
endif()


add_executable(AnimationInjectorSpaceQueryTests EXCLUDE_FROM_ALL tests/SpaceQueryMath.cpp)
target_include_directories(AnimationInjectorSpaceQueryTests PRIVATE src)

add_executable(AnimationInjectorNativeCancelTests EXCLUDE_FROM_ALL tests/NativeCancel.cpp)
target_include_directories(AnimationInjectorNativeCancelTests PRIVATE src)

add_executable(AnimationInjectorLifecycleTests EXCLUDE_FROM_ALL tests/Lifecycle.cpp)
target_include_directories(AnimationInjectorLifecycleTests PRIVATE tests/lifecycle_stubs src)

add_executable(AnimationInjectorGetUpTests EXCLUDE_FROM_ALL tests/GetUpRecovery.cpp)
target_include_directories(AnimationInjectorGetUpTests PRIVATE src)
# A body's side for the flow condition lying (0.12.253).
add_executable(AnimationInjectorLyingSideTests EXCLUDE_FROM_ALL tests/LyingSide.cpp)
target_include_directories(AnimationInjectorLyingSideTests PRIVATE src)
target_compile_features(AnimationInjectorLyingSideTests PRIVATE cxx_std_20)
# An event QTE result starting its action on a body the engine is getting up (0.12.249).
add_executable(AnimationInjectorGetUpHandoffTests EXCLUDE_FROM_ALL tests/GetUpHandoffPlan.cpp)
target_include_directories(AnimationInjectorGetUpHandoffTests PRIVATE src)
target_compile_features(AnimationInjectorGetUpHandoffTests PRIVATE cxx_std_20)

add_executable(AnimationInjectorPoseTrackTests EXCLUDE_FROM_ALL tests/PoseTrackReader.cpp)
target_include_directories(AnimationInjectorPoseTrackTests PRIVATE src)

add_executable(AnimationInjectorRootHandoffTests EXCLUDE_FROM_ALL tests/PairRootHandoff.cpp src/core/PairRootHandoff.cpp)
target_include_directories(AnimationInjectorRootHandoffTests PRIVATE tests/root_handoff_stubs src)
# End position of pairs Animation Injector did not start (0.12.255): when the end begins, exclusions, the slots.
add_executable(AnimationInjectorNativePairEndTests EXCLUDE_FROM_ALL tests/NativePairEndPlan.cpp)
target_include_directories(AnimationInjectorNativePairEndTests PRIVATE src)
target_compile_features(AnimationInjectorNativePairEndTests PRIVATE cxx_std_20)

add_executable(AnimationInjectorMailboxTests EXCLUDE_FROM_ALL tests/BoundedMailbox.cpp)
target_include_directories(AnimationInjectorMailboxTests PRIVATE src)
target_compile_features(AnimationInjectorMailboxTests PRIVATE cxx_std_20)

add_executable(AnimationInjectorFlowLinkRoutesTests EXCLUDE_FROM_ALL tests/FlowLinkRoutes.cpp)
target_include_directories(AnimationInjectorFlowLinkRoutesTests PRIVATE src)

# Exercise the actual input dispatcher with menu and device event doubles.
add_executable(AnimationInjectorInputTests EXCLUDE_FROM_ALL tests/InputSink.cpp src/runtime/InputSink.cpp)
add_executable(AnimationInjectorSuppressionTests EXCLUDE_FROM_ALL tests/EventSuppression.cpp src/runtime/EventSuppression.cpp)
target_include_directories(AnimationInjectorSuppressionTests PRIVATE tests/suppression_stubs src)
target_include_directories(AnimationInjectorInputTests PRIVATE tests/input_stubs src)
if(MSVC)
    target_compile_options(AnimationInjectorInputTests PRIVATE /utf-8)
endif()

# Production registration helpers plus the real parser, compared to frozen 0.12.95 behavior.
add_executable(AnimationInjectorInjectionTests EXCLUDE_FROM_ALL tests/InjectionOptimization.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
add_executable(AnimationInjectorClipRecoveryTests EXCLUDE_FROM_ALL tests/ClipRecovery.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorClipRecoveryTests PRIVATE src)
target_compile_definitions(AnimationInjectorClipRecoveryTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorClipRecoveryTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorClipRecoveryTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
target_compile_options(AnimationInjectorClipRecoveryTests PRIVATE /utf-8)
target_include_directories(AnimationInjectorInjectionTests PRIVATE src)
target_compile_definitions(AnimationInjectorInjectionTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(AnimationInjectorInjectionTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorInjectionTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)
if(MSVC)
    target_compile_options(AnimationInjectorInjectionTests PRIVATE /utf-8)
endif()

add_executable(AnimationInjectorFireArrowTests EXCLUDE_FROM_ALL tests/FireArrow.cpp src/core/FireArrowResolver.cpp src/Settings.cpp src/declarations/XmlV2.cpp src/declarations/ConditionFlowXml.cpp)
target_include_directories(AnimationInjectorFireArrowTests PRIVATE src "${AI_HDE64_SOURCE_DIR}/src/hde")
target_compile_definitions(AnimationInjectorFireArrowTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AnimationInjectorFireArrowTests PRIVATE /utf-8)
target_precompile_headers(AnimationInjectorFireArrowTests PRIVATE src/PCH.h)
target_link_libraries(AnimationInjectorFireArrowTests PRIVATE CommonLibSSE::CommonLibSSE pugixml::pugixml)

# R-13: every unit test from one command.
#   cmake --build build --config Release --target AnimationInjectorAllTests
#   ctest --test-dir build -C Release --output-on-failure
# A new AnimationInjector<Name>Tests target is registered automatically as test <Name>
# without arguments. Tests that need inputs are listed below; their fixtures live in tests/data.
enable_testing()
set(AI_TEST_DATA "${CMAKE_SOURCE_DIR}/tests/data")
# The Dynamic Dodge Shot port (the event shot is a registry function).
set(AI_DDS_PORT "${AI_TEST_DATA}/dynamic-dodge-shot.xml")
file(GLOB AI_PORT_DECLARATIONS "${CMAKE_SOURCE_DIR}/ports/*/SKSE/Plugins/AnimationInjector/*/*.xml")
set(AI_TESTS_WITH_INPUTS Carry PairInput PairMotion FireArrow RuntimeExtension ClipRecovery ClipIndex GrappleFlow)

get_property(AI_ALL_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(AI_TEST_TARGETS AnimationInjectorXmlCheck)
foreach(target IN LISTS AI_ALL_TARGETS)
    if(target MATCHES "^AnimationInjector(.+)Tests$")
        list(APPEND AI_TEST_TARGETS ${target})
        if(NOT CMAKE_MATCH_1 IN_LIST AI_TESTS_WITH_INPUTS)
            add_test(NAME ${CMAKE_MATCH_1} COMMAND ${target})
        endif()
    endif()
endforeach()
add_custom_target(AnimationInjectorAllTests DEPENDS ${AI_TEST_TARGETS})

# tests/data/*.bin are captured game code and stay out of the public source. Without them Carry and FireArrow
# skip their native checks, and PairInput/PairMotion (native checks only) are not registered.
set(AI_NATIVE_CARRY "${AI_TEST_DATA}/carry-sync-functions.bin")
set(AI_NATIVE_PAIR_INPUT "${AI_TEST_DATA}/pair-input-function.bin")
set(AI_NATIVE_PAIR_MOTION "${AI_TEST_DATA}/pair-motion-function.bin")
set(AI_NATIVE_FIRE_ARROW "${AI_TEST_DATA}/fire-arrow-native-ae.bin")
if(EXISTS "${AI_NATIVE_CARRY}")
    add_test(NAME Carry COMMAND AnimationInjectorCarryTests "${AI_NATIVE_CARRY}")
else()
    add_test(NAME Carry COMMAND AnimationInjectorCarryTests)
endif()
if(EXISTS "${AI_NATIVE_PAIR_INPUT}")
    add_test(NAME PairInput COMMAND AnimationInjectorPairInputTests "${AI_NATIVE_PAIR_INPUT}")
endif()
if(EXISTS "${AI_NATIVE_PAIR_MOTION}")
    add_test(NAME PairMotion COMMAND AnimationInjectorPairMotionTests "${AI_NATIVE_PAIR_MOTION}")
endif()
if(EXISTS "${AI_NATIVE_FIRE_ARROW}")
    add_test(NAME FireArrow COMMAND AnimationInjectorFireArrowTests "${AI_DDS_PORT}" "${AI_NATIVE_FIRE_ARROW}")
else()
    add_test(NAME FireArrow COMMAND AnimationInjectorFireArrowTests "${AI_DDS_PORT}")
endif()
if(NOT EXISTS "${AI_NATIVE_CARRY}" OR NOT EXISTS "${AI_NATIVE_FIRE_ARROW}")
    message(STATUS "Native game-code fixtures absent: their checks are skipped")
endif()
add_test(NAME RuntimeExtension COMMAND AnimationInjectorRuntimeExtensionTests "${AI_DDS_PORT}")
add_test(NAME ClipRecovery COMMAND AnimationInjectorClipRecoveryTests "${AI_TEST_DATA}/clip-recovery-roundtrip.xml")
# The ports (other authors' mods, not in the public source) give the most clips; the test fixtures stand in.
if(NOT AI_PORT_DECLARATIONS)
    set(AI_PORT_DECLARATIONS "${AI_DDS_PORT}" "${AI_TEST_DATA}/grapple.xml")
endif()
add_test(NAME ClipIndex COMMAND AnimationInjectorClipIndexTests "${AI_TEST_DATA}/vanilla-clip-names.txt" ${AI_PORT_DECLARATIONS})
add_test(NAME XmlCheck COMMAND AnimationInjectorXmlCheck --json "${AI_DDS_PORT}")
# --compare: a branch into <Stop> equals a missing one (the editor's form); a flow that only stops
# still differs from no flow.
add_test(NAME XmlCompareFlowStop COMMAND AnimationInjectorXmlCheck --compare
    "${AI_TEST_DATA}/flow-stop/stop.xml" "${AI_TEST_DATA}/flow-stop/folded.xml")
add_test(NAME XmlCompareFlowStopOnly COMMAND AnimationInjectorXmlCheck --compare
    "${AI_TEST_DATA}/flow-stop/folded.xml" "${AI_TEST_DATA}/flow-stop/no-flow.xml")
set_tests_properties(XmlCompareFlowStopOnly PROPERTIES PASS_REGULAR_EXPRESSION "Semantic mismatch: Never")
# --compare walks a flow from its entry: the order nodes are written in and their IDs do not count (the editor
# writes a Play right after the branch reaching it), the order of a group's members does.
add_test(NAME XmlCompareFlowOrder COMMAND AnimationInjectorXmlCheck --compare
    "${AI_TEST_DATA}/flow-order/written.xml" "${AI_TEST_DATA}/flow-order/editor.xml")
add_test(NAME XmlCompareFlowMembers COMMAND AnimationInjectorXmlCheck --compare
    "${AI_TEST_DATA}/flow-order/written.xml" "${AI_TEST_DATA}/flow-order/members-swapped.xml")
set_tests_properties(XmlCompareFlowMembers PROPERTIES PASS_REGULAR_EXPRESSION "Semantic mismatch: Back")
add_test(NAME RagdollProfile COMMAND AnimationInjectorXmlCheck --ragdoll "${AI_TEST_DATA}/ragdoll/chicken.xml")
add_test(NAME RagdollProfileReject COMMAND AnimationInjectorXmlCheck --ragdoll-reject "${AI_TEST_DATA}/ragdoll/invalid.xml")
# Which profile a carry reads: the lift clip's own (Ragdoll/<clip file name>.xml), else the rig's.
function(ai_ragdoll_resolve name rig clip expected)
    add_test(NAME RagdollResolve-${name}
        COMMAND AnimationInjectorXmlCheck --ragdoll-resolve "${AI_TEST_DATA}/ragdoll-resolve/Data" ${rig} "${clip}")
    set_tests_properties(RagdollResolve-${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
endfunction()
ai_ragdoll_resolve(clip character "Animations/Grapple/Grapple_Lift.hkx"
    "Resolved: clip SKSE/Plugins/AnimationInjector/Ragdoll/Grapple_Lift.xml bodies=2")
ai_ragdoll_resolve(clip-as-declared character "Animations\\Grapple\\GRAPPLE_LIFT.HKX" "Resolved: clip .* bodies=2")
ai_ragdoll_resolve(clip-any-folder character "Other/Grapple_Lift.hkx" "Resolved: clip .*/Grapple_Lift.xml bodies=2")
ai_ragdoll_resolve(clip-no-folder character "Grapple_Lift.hkx" "Resolved: clip .*/Grapple_Lift.xml bodies=2")
ai_ragdoll_resolve(no-clip-file character "Animations/Other/None.hkx"
    "Resolved: rig SKSE/Plugins/AnimationInjector/Ragdoll/character.xml bodies=0")
ai_ragdoll_resolve(clip-other-rig character "Animations/Wrong/Wrong_Lift.hkx" "Resolved: rig .*/character.xml")
ai_ragdoll_resolve(clip-rejected character "Animations/Broken/Broken_Lift.hkx" "Resolved: rig .*/character.xml")
# A clip named like a rig profile (a runtime rig, or a rig only HKX_Edit saves) never reads it as its own.
ai_ragdoll_resolve(rig-name character "Animations/Lifts/Wolf.hkx" "Resolved: rig .*/character.xml")
ai_ragdoll_resolve(hkx-edit-rig-name character "Animations/Lifts/character_female.hkx" "Resolved: rig .*/character.xml")
ai_ragdoll_resolve(empty-name character "Animations/Lifts/.hkx" "Resolved: rig .*/character.xml")
ai_ragdoll_resolve(not-hkx character "Animations/Grapple/Grapple_Lift.xml" "Resolved: rig .*/character.xml")
ai_ragdoll_resolve(no-rig-file chicken "Animations/Grapple/Grapple_Lift.hkx" "Resolved: none")
# Carry putdown: a carrier-only Action with a CarryRelease moment (docs/설계-운반-래그돌-내려놓기.md).
add_test(NAME CarryPutdownXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/carry/putdown.xml")
add_test(NAME CarryPutdownEventXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/carry/putdown-event.xml")
foreach(case release no-release-moment putdown-start release-outside-putdown putdown-pair)
    add_test(NAME CarryPutdownReject-${case}
        COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/carry/reject-${case}.xml")
endforeach()
# P-2: the installed INI is the template's layout with every user value kept.
add_test(NAME IniMerge COMMAND ${CMAKE_COMMAND} -D "TEMPLATE=${CMAKE_SOURCE_DIR}/misc/AnimationInjector.ini"
    -D "OLD=${AI_TEST_DATA}/ini/user-old.ini" -D "EXPECTED=${AI_TEST_DATA}/ini/merged.ini"
    -D "WORK=${CMAKE_BINARY_DIR}/ini-merge-test" -P "${CMAKE_SOURCE_DIR}/tests/IniMerge.cmake")
# Format 2/3 -> 4 converter (tools/convert_v4.py): golden bytes, the result parses, combos refused.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME ConvertV4 COMMAND ${CMAKE_COMMAND} -D "PYTHON=${Python3_EXECUTABLE}"
        -D "CONVERTER=${CMAKE_SOURCE_DIR}/tools/convert_v4.py" -D "CHECK=$<TARGET_FILE:AnimationInjectorXmlCheck>"
        -D "DATA=${AI_TEST_DATA}/convert-v4" -D "WORK=${CMAKE_BINARY_DIR}/convert-v4-test"
        -P "${CMAKE_SOURCE_DIR}/tests/ConvertV4.cmake")
else()
    message(WARNING "Python 3 not found: the ConvertV4 test is unavailable")
endif()
# Carry fix moment: <Fix at="seconds"/> inside Carry (docs/설계-운반-개편.md).
add_test(NAME CarryFixXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/carry/fix.xml")
foreach(case fix-event fix-negative fix-duplicate fix-missing)
    add_test(NAME CarryFixReject-${case}
        COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/carry/reject-${case}.xml")
endforeach()
# Carry weapon is "sheathed" or "sheathe"; any other value is refused.
add_test(NAME CarryWeaponReject-weapon-mode
    COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/carry/reject-weapon-mode.xml")
# Removed in 0.12.238 (unused by every installed declaration): Carry weapon="keep" and weaponShow, the
# Lock window with the lock/unlock event functions, and External. Each refused for that reason.
foreach(case carry/reject-weapon-keep carry/reject-weapon-show removed/lock-window removed/lock-function
        removed/external)
    string(REPLACE "/" "-" name "${case}")
    add_test(NAME Removed0238-${name}
        COMMAND AnimationInjectorXmlCheck --json "${AI_TEST_DATA}/${case}.xml")
    set_tests_properties(Removed0238-${name} PROPERTIES PASS_REGULAR_EXPRESSION "\"valid\":false.*0\\.12\\.238")
endforeach()
# Removed in 0.12.255: the per-pair EndPosition (the end position applies to every pair, INI iPairEndPosition).
add_test(NAME Removed0255-end-position
    COMMAND AnimationInjectorXmlCheck --json "${AI_TEST_DATA}/removed/end-position.xml")
set_tests_properties(Removed0255-end-position PROPERTIES PASS_REGULAR_EXPRESSION "\"valid\":false.*0\\.12\\.255")
# <Hold pose> is "lift-end" only (carry stage 2b, an HKX hold pose, was dropped).
foreach(case hold-pose hold-time)
    add_test(NAME CarryHoldReject-${case}
        COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/carry/reject-${case}.xml")
endforeach()
# weapon="sheathe": put away with no animation as the lift starts. The printed Carry line (valid
# files only) proves the value reaches the runtime spec.
add_test(NAME CarryWeaponSheatheXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/carry/weapon-sheathe.xml")
set_tests_properties(CarryWeaponSheatheXml PROPERTIES PASS_REGULAR_EXPRESSION "weapon=sheathe")
# HKX paths are printable ASCII without the characters Windows forbids: the engine opens animation files
# through code-page paths (a Korean name is never found). The reason test proves which check rejected it.
add_test(NAME HkxPathSymbolsXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/hkx-path/symbols.xml")
foreach(case hangul star)
    add_test(NAME HkxPathReject-${case}
        COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/hkx-path/reject-${case}.xml")
    add_test(NAME HkxPathReason-${case}
        COMMAND AnimationInjectorXmlCheck --json "${AI_TEST_DATA}/hkx-path/reject-${case}.xml")
    set_tests_properties(HkxPathReason-${case} PROPERTIES PASS_REGULAR_EXPRESSION "\"valid\":false.*ASCII")
endforeach()
# A selecting action may leave its clip out (0.12.244): only with a Flow that never plays the action itself,
# and only Start, Require, Suppress and Flow. The reason tests prove which check rejected each file.
add_test(NAME SelectingNoClipXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/selecting/selecting.xml")
foreach(item "no-flow|0[.]12[.]244" "self|SelfPick" "extra|Flow.*: Enter")
    string(REPLACE "|" ";" parts "${item}")
    list(GET parts 0 case)
    list(GET parts 1 reason)
    add_test(NAME SelectingNoClipReject-${case}
        COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/selecting/reject-${case}.xml")
    add_test(NAME SelectingNoClipReason-${case}
        COMMAND AnimationInjectorXmlCheck --json "${AI_TEST_DATA}/selecting/reject-${case}.xml")
    set_tests_properties(SelectingNoClipReason-${case} PROPERTIES PASS_REGULAR_EXPRESSION "\"valid\":false.*${reason}")
endforeach()
# A QTE started by its event (0.12.248, <EventRegistry><Event><QTE>): the ragdoll recovery declaration reads; an
# event's QTE has no start time and no recipient, starting while down is gone, and a result's actions (0.12.249, the
# partner's 0.12.264) are ordinary actions of the same file. The reason tests prove which check rejected each file.
add_test(NAME EventQteXml COMMAND AnimationInjectorXmlCheck "${AI_TEST_DATA}/eventqte/ragdoll.xml")
foreach(item "at|'at'" "pairto|: Hug" "missing|: Nowhere" "partnerto|: Hug" "partner|'recipient'" "recover|'ragdoll'" "lying|invalid option")
    string(REPLACE "|" ";" parts "${item}")
    list(GET parts 0 case)
    list(GET parts 1 reason)
    add_test(NAME EventQteReject-${case}
        COMMAND AnimationInjectorXmlCheck --reject "${AI_TEST_DATA}/eventqte/reject-${case}.xml")
    add_test(NAME EventQteReason-${case}
        COMMAND AnimationInjectorXmlCheck --json "${AI_TEST_DATA}/eventqte/reject-${case}.xml")
    set_tests_properties(EventQteReason-${case} PROPERTIES PASS_REGULAR_EXPRESSION "\"valid\":false.*${reason}")
endforeach()
# Every combination of Grapple's routing inputs against the expected destinations (air and
# equipped-type branches included) on a fixed copy of the Grapple declaration.
add_test(NAME GrappleFlow COMMAND AnimationInjectorGrappleFlowTests "${AI_TEST_DATA}/grapple.xml")

# E-8: runtime C++ layout (.clang-format). Visual Studio ships clang-format next to MSVC.
get_filename_component(AI_MSVC_TOOLS "${CMAKE_CXX_COMPILER}/../../../../../.." ABSOLUTE)
find_program(AI_CLANG_FORMAT clang-format HINTS "${AI_MSVC_TOOLS}/Llvm/x64/bin")
file(GLOB_RECURSE AI_FORMATTED_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h tests/*.cpp tests/*.h)
if(AI_CLANG_FORMAT)
    add_test(NAME Format COMMAND "${AI_CLANG_FORMAT}" --dry-run --Werror ${AI_FORMATTED_SOURCES})
    add_custom_target(AnimationInjectorFormat COMMAND "${AI_CLANG_FORMAT}" -i ${AI_FORMATTED_SOURCES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" COMMENT "Formatting runtime sources")
else()
    message(WARNING "clang-format not found: the Format test and AnimationInjectorFormat target are unavailable")
endif()
