function(ss_register_test name labels timeout)
    if(NOT TARGET ${name})
        message(FATAL_ERROR "Required test target is missing: ${name}")
    endif()
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}"
        "-DSS_TEST_NAME=${name}"
        "-DSS_TEST_EXECUTABLE=$<TARGET_FILE:${name}>"
        "-DSS_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/Testing/Artifacts"
        "-DSS_TEST_ARGS=${ARGN}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SsRunTest.cmake")
    set_tests_properties(${name} PROPERTIES LABELS "${labels}" TIMEOUT ${timeout})
endfunction()

foreach(SS_TEST IN ITEMS source_path frame_motion_test frame_select_test
        mesh_format_roundtrip delaunay_degenerate device_lease_test agent_state_test
        agent_config_test agent_wire_test worker_request_test checkpoint_resolution_test
        agent_pairing_test agent_transfer_test dataset_parser_test
        project_manifest_test data_manager_sampler_test step_config_test ply_reader_bounds_test
        agent_training_job_test agent_update_package_test agent_admin_intent_test)
    ss_register_test(${SS_TEST} "headless;fast" 60)
endforeach()
ss_register_test(agent_leader_test "headless;fast" 180)

ss_register_test(cli_training_smoke "gpu" 600 "$<TARGET_FILE:spirula>")
set_tests_properties(cli_training_smoke PROPERTIES RESOURCE_LOCK training_gpu)
add_dependencies(cli_training_smoke spirula)
ss_register_test(scheduler_test "headless;worker" 300 "$<TARGET_FILE:spirula>")
ss_register_test(subprocess_test "headless;worker" 300)
ss_register_test(scheduler_result_test "headless;worker" 60)
foreach(SS_TEST IN ITEMS split_faces_test frustum_size_test bilagrid_selector_test)
    if(TARGET ${SS_TEST})
        ss_register_test(${SS_TEST} "headless;fast" 60)
    else()
        message(STATUS "Headless subset: ${SS_TEST} requires SS_BUILD_BACKEND_TESTS on CUDA")
    endif()
endforeach()
add_dependencies(scheduler_test spirula)

if(SS_BUILD_GUI)
    foreach(SS_TEST IN ITEMS command_argv_test preset_roundtrip_test
            recon_stamp_test frames_stamp_test)
        ss_register_test(${SS_TEST} "headless;fast" 60)
    endforeach()
    ss_register_test(batch_process_test "headless;worker" 60 "$<TARGET_FILE:spirula>")
    add_dependencies(batch_process_test spirula)
    message(STATUS "Headless tests: core plus GUI-adjacent data tests (no window)")
else()
    message(STATUS "Headless tests: core subset; batch/preset/stamp/argv tests require SS_BUILD_GUI")

endif()
if(SS_BUILD_SFM)
    ss_register_test(sfm_feature_work_test "headless;fast" 60)
    ss_register_test(agent_feature_job_test "headless;fast" 60)
    ss_register_test(agent_feature_worker_store_test "headless;fast" 60)
    ss_register_test(agent_portable_worker_store_test "headless;fast" 60)
    ss_register_test(agent_reconstruction_job_test "headless;fast" 60)
endif()

if(SS_ENABLE_PATENTED)
    ss_register_test(mkv_timing_test "gpu;native_video" 120
        "${CMAKE_CURRENT_SOURCE_DIR}/src/video/tests/fixtures")
    set_tests_properties(mkv_timing_test PROPERTIES RESOURCE_LOCK training_gpu)
endif()
