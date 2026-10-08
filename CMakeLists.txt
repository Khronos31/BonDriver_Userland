cmake_minimum_required(VERSION 3.16)
project(BonDriver_Userland CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Source and test literals contain UTF-8 (channel names); MSVC must not treat
# them as the active code page.
if(MSVC)
	add_compile_options(/utf-8)
endif()

include(GNUInstallDirs)

option(BUILD_TESTING "Build the test suite" ON)
option(BONDRIVER_WERROR "Treat warnings as errors" OFF)

add_library(bondriver_common STATIC
	src/common/Config.cpp
	src/common/TsBuffer.cpp
	src/common/FileLock.cpp
	src/common/Utf16.cpp
	src/common/DebugLog.cpp
	src/common/ModulePath.cpp
	src/common/PipelineCommon.cpp
)
target_include_directories(bondriver_common PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:include>
)
set_target_properties(bondriver_common PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES)

if(BUILD_TESTING)
	target_compile_definitions(bondriver_common PRIVATE BONDRIVER_ENABLE_TEST_FAULTS)
endif()

if(WIN32)
	target_sources(bondriver_common PRIVATE src/common/PipelineWin.cpp)
	# MSVC marks standard CRT calls such as getenv/strerror deprecated even
	# when used portably. Keep /W4 /WX effective for all other diagnostics.
	target_compile_definitions(bondriver_common PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_sources(bondriver_common PRIVATE src/common/PipelinePosix.cpp)
	target_link_libraries(bondriver_common PUBLIC ${CMAKE_DL_LIBS})
endif()

if(BONDRIVER_WERROR)
	if(MSVC)
		target_compile_options(bondriver_common PRIVATE /W4 /WX)
	else()
		target_compile_options(bondriver_common PRIVATE -Wall -Wextra -Werror)
	endif()
endif()

# Shared libraries.  No "lib" prefix so EDCB/TVTest load the documented names.
add_library(BonDriver_Siano SHARED
	src/common/DriverCore.cpp
	src/siano/SianoBackend.cpp
	src/siano/exports.cpp
)
target_link_libraries(BonDriver_Siano PRIVATE bondriver_common)
set_target_properties(BonDriver_Siano PROPERTIES
	PREFIX ""
	OUTPUT_NAME "BonDriver_Siano"
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES)

add_library(BonDriver_PX4 SHARED
	src/common/DriverCore.cpp
	src/px4/Px4Backend.cpp
	src/px4/exports.cpp
)
target_link_libraries(BonDriver_PX4 PRIVATE bondriver_common)
set_target_properties(BonDriver_PX4 PROPERTIES
	PREFIX ""
	OUTPUT_NAME "BonDriver_PX4"
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES)

if(WIN32)
	# A .def file keeps CreateBonDriver/CreateBonStruct undecorated on x86 as
	# well as x64 (x86 cdecl __declspec(dllexport) would decorate them).
	target_sources(BonDriver_Siano PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BonDriver_Siano.def)
	target_compile_definitions(BonDriver_Siano PRIVATE BONDRIVER_USING_DEF)
	target_sources(BonDriver_PX4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BonDriver_PX4.def)
	target_compile_definitions(BonDriver_PX4 PRIVATE BONDRIVER_USING_DEF)
endif()

install(TARGETS BonDriver_Siano BonDriver_PX4
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
	config/BonDriver_Siano.ini.example
	config/BonDriver_PX4.ini.example
	config/channels-ground.example.tsv
	config/channels-satellite.example.tsv
	config/channels-catv.example.tsv
	DESTINATION ${CMAKE_INSTALL_DATADIR}/BonDriver_Userland)

if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()
