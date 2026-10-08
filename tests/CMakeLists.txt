find_package(Threads REQUIRED)

add_executable(fake_cli fake_cli/fake_cli.cpp)
target_include_directories(fake_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
	target_link_libraries(fake_cli PRIVATE Threads::Threads)
endif()

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE bondriver_common)

add_executable(test_buffer test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE bondriver_common)

add_executable(test_process test_process.cpp)
target_link_libraries(test_process PRIVATE bondriver_common Threads::Threads)

add_executable(test_driver test_driver.cpp)
target_include_directories(test_driver PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(NOT WIN32)
	target_link_libraries(test_driver PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

add_executable(abi_consumer abi_consumer/consumer_main.cpp)
target_include_directories(abi_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi_consumer)
if(NOT WIN32)
	target_link_libraries(abi_consumer PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(interop_consumer interop_consumer.cpp)
target_include_directories(interop_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi_consumer)
if(NOT WIN32)
	target_link_libraries(interop_consumer PRIVATE ${CMAKE_DL_LIBS})
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
	cleanup_failure)
set(BONDRIVER_BACKENDS "siano:BonDriver_Siano" "px4:BonDriver_PX4")
foreach(pair ${BONDRIVER_BACKENDS})
	string(REPLACE ":" ";" fields ${pair})
	list(GET fields 0 backend)
	list(GET fields 1 target)
	foreach(scenario ${BONDRIVER_SCENARIOS})
		add_test(NAME driver_${backend}_${scenario}
			COMMAND test_driver $<TARGET_FILE:${target}> ${backend} $<TARGET_FILE:fake_cli> ${scenario})
	endforeach()
endforeach()

# Canonical runtime-path alias contention is a PX4 identity concern only.
add_test(NAME driver_px4_runtime_alias
	COMMAND test_driver $<TARGET_FILE:BonDriver_PX4> px4 $<TARGET_FILE:fake_cli> runtime_alias)

add_test(NAME abi_consumer
	COMMAND abi_consumer $<TARGET_FILE:fake_cli> $<TARGET_FILE:BonDriver_Siano> $<TARGET_FILE:BonDriver_PX4>)
add_test(NAME interop_consumer
	COMMAND interop_consumer $<TARGET_FILE:fake_cli> $<TARGET_FILE:BonDriver_Siano> $<TARGET_FILE:BonDriver_PX4>)

if(WIN32)
	add_test(NAME windows_exports
		COMMAND ${CMAKE_COMMAND}
			-DLIBS=$<TARGET_FILE:BonDriver_Siano>|$<TARGET_FILE:BonDriver_PX4>
			-DSYS=Windows
			-DLINKER=${CMAKE_LINKER}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/check_exports.cmake)
elseif(UNIX)
	add_test(NAME shared_exports
		COMMAND ${CMAKE_COMMAND}
			-DLIBS=$<TARGET_FILE:BonDriver_Siano>|$<TARGET_FILE:BonDriver_PX4>
			-DSYS=${CMAKE_SYSTEM_NAME}
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
