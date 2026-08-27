set(UNICORN_REGRESSION_TARGETS)

function(add_unicorn_regression test_name architecture)
    string(MAKE_C_IDENTIFIER "regress_${test_name}" test_target)
    add_executable(${test_target}
        ${CMAKE_CURRENT_LIST_DIR}/${test_name}.c
    )
    target_compile_options(${test_target} PRIVATE
        ${UNICORN_COMPILE_OPTIONS}
    )
    target_link_libraries(${test_target} PRIVATE
        ${SAMPLES_LIB}
    )
    add_test(NAME regression.${test_name} COMMAND ${test_target})
    set_tests_properties(regression.${test_name} PROPERTIES
        LABELS "regression;${architecture}"
        TIMEOUT 30
    )
    if(ANDROID_ABI)
        file(APPEND ${CMAKE_BINARY_DIR}/adb.sh
            "adb push ${test_target} /data/local/tmp/build/\n")
        file(APPEND ${CMAKE_BINARY_DIR}/adb.sh
            "adb shell \"chmod +x /data/local/tmp/build/${test_target}\"\n")
        file(APPEND ${CMAKE_BINARY_DIR}/adb.sh
            "adb shell 'LD_LIBRARY_PATH=/data/local/tmp/build:$LD_LIBRARY_PATH /data/local/tmp/build/${test_target}' || exit -1\n")
    endif()
    list(APPEND UNICORN_REGRESSION_TARGETS ${test_target})
    set(UNICORN_REGRESSION_TARGETS ${UNICORN_REGRESSION_TARGETS}
        PARENT_SCOPE)
endfunction()

if(UNICORN_HAS_X86)
    set(X86_REGRESSION_TESTS
        00opcode_uc_crash
        block_test
        cross_page_smc
        eflags_noset
        eflags_nosync
        emu_clear_errors
        hook_extrainvoke
        invalid_read_in_cpu_tb_exec
        invalid_read_in_tb_flush_x86_64
        invalid_write_in_cpu_tb_exec_x86_64
        map_crash
        map_write
        memory_hook_stop
        nested_stop_state
        nr_mem_test
        rep_movsb
        ro_mem_test
        rw_hookstack
        sigill
        sigill2
        sysenter_hook_x86
        x86_16_segfault
        x86_vex
    )
    foreach(TEST_NAME ${X86_REGRESSION_TESTS})
        add_unicorn_regression(${TEST_NAME} x86)
    endforeach()
endif()

if(UNICORN_HAS_ARM)
    set(ARM_REGRESSION_TESTS
        001-bad_condition_code_0xe
        004-segmentation_fault_1
        arm_enable_vfp
        timeout_segfault
    )
    foreach(TEST_NAME ${ARM_REGRESSION_TESTS})
        add_unicorn_regression(${TEST_NAME} arm)
    endforeach()
endif()

if(UNICORN_HAS_M68K)
    set(M68K_REGRESSION_TESTS
        002-qemu__fatal__unimplemented_control_register_write_0xffb___0x0
        003-qemu__fatal__wdebug_not_implemented
        005-qemu__fatal__illegal_instruction__0000___00000404
        006-qemu__fatal__illegal_instruction__0421___00040026
    )
    foreach(TEST_NAME ${M68K_REGRESSION_TESTS})
        add_unicorn_regression(${TEST_NAME} m68k)
    endforeach()
endif()

if(UNICORN_HAS_MIPS)
    set(MIPS_REGRESSION_TESTS
        emu_stop_in_hook_overrun
        mips_branch_likely_issue
        mips_delay_slot_code_hook
        mips_invalid_read_of_size_4_when_tracing
        mips_kseg0_1
        threaded_emu_start
    )
    foreach(TEST_NAME ${MIPS_REGRESSION_TESTS})
        add_unicorn_regression(${TEST_NAME} mips)
    endforeach()
    set_tests_properties(regression.threaded_emu_start PROPERTIES TIMEOUT 45)
endif()

if(UNICORN_HAS_SPARC)
    add_unicorn_regression(sparc_jump_to_zero sparc)
endif()
