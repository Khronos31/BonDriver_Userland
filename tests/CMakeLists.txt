find_package(Threads REQUIRED)

add_executable(fake_cli fake_cli/fake_cli.cpp)
target_include_directories(fake_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
	target_link_libraries(fake_cli PRIVATE Threads::Threads)
endif()

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE bondriver_common)
target_compile_definitions(test_config PRIVATE BONDRIVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(test_buffer test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE bondriver_common)

add_executable(test_process test_process.cpp)
target_link_libraries(test_process PRIVATE bondriver_common Threads::Threads)

add_executable(test_driver test_driver.cpp)
target_include_directories(test_driver PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(test_groups test_groups.cpp)
target_include_directories(test_groups PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi_consumer)
if(NOT WIN32)
	target_link_libraries(test_groups PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

if(MSVC AND BONDRIVER_STATIC_CRT)
	set(_bondriver_test_static_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	foreach(target IN ITEMS test_config test_buffer test_process)
		set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "${_bondriver_test_static_crt}")
	endforeach()
endif()

if(NOT WIN32)
	target_link_libraries(test_driver PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

add_executable(abi_consumer abi_consumer/consumer_main.cpp)
target_include_directories(abi_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi_consumer)
if(NOT WIN32)
	target_link_libraries(abi_consumer PRIVATE ${CMAKE_DL_LIBS})
endif()

add_test(NAME unit_buffer COMMAND test_buffer)
add_test(NAME unit_config COMMAND test_config)
add_test(NAME unit_process COMMAND test_process $<TARGET_FILE:fake_cli>)

set(BONDRIVER_SCENARIOS
	basic
	startup_timeout
	dead_child
	lease
	multiple_factory
	struct
	purge_partial
	buffer_overflow
	multiple_buffers
	wait_close
	invalid_config
	process_environment
	factory_failure
	thread_start_failure
	thread_start_cleanup_failure
	cleanup_failure)
foreach(scenario ${BONDRIVER_SCENARIOS})
	add_test(NAME driver_userland_${scenario}
		COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> ${scenario})
endforeach()

add_test(NAME driver_userland_runtime_alias
	COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> runtime_alias)

# Acceptance: real child process, exact argv, child failure, pool and
# cross-process exclusivity.  The pool/exclusivity/daemon-control scenarios use
# fork and are POSIX-only; Windows still builds the same test binary.
add_test(NAME driver_userland_argv_contract_t
	COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> argv_contract_t)
add_test(NAME driver_userland_child_failure
	COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> child_failure)
add_test(NAME driver_userland_argv_contract_s
	COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> argv_contract_s)
if(NOT WIN32)
	add_test(NAME driver_userland_pool_two_processes
		COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> pool_two_processes)
	add_test(NAME driver_userland_pool_exhaust_release
		COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> pool_exhaust_release)
	add_test(NAME driver_userland_physical_key_labels
		COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> physical_key_labels)
	add_test(NAME driver_userland_no_daemon_control
		COMMAND test_driver $<TARGET_FILE:BonDriver_Userland> userland $<TARGET_FILE:fake_cli> no_daemon_control)
endif()

add_test(NAME group_select
	COMMAND test_groups $<TARGET_FILE:BonDriver_Userland> $<TARGET_FILE:fake_cli>)

add_test(NAME abi_consumer
	COMMAND abi_consumer $<TARGET_FILE:fake_cli> $<TARGET_FILE:BonDriver_Userland>)

if(WIN32)
	add_test(NAME windows_exports
		COMMAND ${CMAKE_COMMAND}
			-DLIBS=$<TARGET_FILE:BonDriver_Userland>
			-DSYS=Windows
			-DLINKER=${CMAKE_LINKER}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/check_exports.cmake)
elseif(UNIX)
	add_test(NAME shared_exports
		COMMAND ${CMAKE_COMMAND}
			-DLIBS=$<TARGET_FILE:BonDriver_Userland>
			-DSYS=${CMAKE_SYSTEM_NAME}
			-DSTATIC_CXX=${BONDRIVER_STATIC_CXX_RUNTIME}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/check_exports.cmake)
endif()

# Optional: compile an independent consumer against a real EDCB BonCtrl
# checkout.  Enable with -DBONDRIVER_EDCB_INCLUDE_DIR=<path>.
if(BONDRIVER_EDCB_INCLUDE_DIR)
	add_executable(edcb_compile_check edcb_compile_check.cpp)
	target_include_directories(edcb_compile_check PRIVATE ${BONDRIVER_EDCB_INCLUDE_DIR})
	add_test(NAME edcb_compile_check COMMAND edcb_compile_check)
endif()

# Every behavioural test must have a hard timeout so a hung wait can never
# stall the suite.
get_property(BONDRIVER_ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${BONDRIVER_ALL_TESTS} PROPERTIES TIMEOUT 90)
set_tests_properties(unit_process PROPERTIES TIMEOUT 15)
