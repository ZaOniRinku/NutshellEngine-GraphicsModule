cmake_minimum_required(VERSION 3.1 FATAL_ERROR)
set(NAME NutshellGraphicsModule)
project(${NAME} VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NTSH_DEFINES "")

set(NTSHENGN_COMMON_PATH "NtshEngn_Common_NOTFOUND" CACHE PATH "The directory containing NutshellEngine's common resources.")
if(NOT ${NTSHENGN_COMMON_PATH} STREQUAL "NtshEngn_Common_NOTFOUND" AND EXISTS ${NTSHENGN_COMMON_PATH})
	file(COPY ${NTSHENGN_COMMON_PATH}/ DESTINATION ${CMAKE_SOURCE_DIR}/external/Common)
	message(STATUS "Copy NutshellEngine's common resources from ${NTSHENGN_COMMON_PATH} to ${CMAKE_SOURCE_DIR}/external/Common")
else()
	message(SEND_ERROR "Could not find the directory containing NutshellEngine's common resources.")
endif()

add_subdirectory(external/Common)

set(NTSHENGN_MODULE_PATH "NtshEngn_Module_NOTFOUND" CACHE PATH "The directory containing NutshellEngine's common module resources.")
if(NOT ${NTSHENGN_MODULE_PATH} STREQUAL "NtshEngn_Module_NOTFOUND" AND EXISTS ${NTSHENGN_MODULE_PATH})
	file(COPY ${NTSHENGN_MODULE_PATH}/ DESTINATION ${CMAKE_SOURCE_DIR}/external/Module)
	message(STATUS "Copy NutshellEngine's common module resources from ${NTSHENGN_MODULE_PATH} to ${CMAKE_SOURCE_DIR}/external/Module")
else()
	message(SEND_ERROR "Could not find the directory containing NutshellEngine's common module resources.")
endif()

add_subdirectory(external/Module)

add_subdirectory(external/glad)

find_package(OpenGL)

if(NOT OpenGL_FOUND)
	message(FATAL_ERROR "Could not find OpenGL library.")
endif()

list(APPEND NTSH_DEFINES NTSH_CURRENT_MODULE_GRAPHICS)

set(MODULE_SOURCES
	src/ntsh_graphics_module.cpp)	
set(MODULE_HEADERS
	src/ntsh_graphics_module.h)

set(SOURCES ${MODULE_SOURCES})
set(HEADERS ${MODULE_HEADERS})

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_link_libraries(${PROJECT_NAME} PRIVATE NutshellEngine::Module NutshellEngine::Common opengl32 GLAD)

target_compile_options(${PROJECT_NAME} PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
	$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Werror>)

target_compile_definitions(${PROJECT_NAME} PRIVATE
	${NTSH_DEFINES}
	PUBLIC
	$<$<CONFIG:Debug>:NTSH_DEBUG>
	$<$<CONFIG:MinSizeRel>:NTSH_RELEASE>
	$<$<CONFIG:Release>:NTSH_RELEASE>
	$<$<CONFIG:RelWithDebInfo>:NTSH_DEBUG>)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    DEFINE_SYMBOL "NutshellModule_EXPORTS")

set(NTSHENGN_INSTALL_MODULE_PATH "NtshEngn_InstallModule_NOTFOUND" CACHE PATH "Give the option to install the built module into NutshellEngine\'s modules directory.")
if(EXISTS ${NTSHENGN_INSTALL_MODULE_PATH})
	message(STATUS "Found NutshellEngine\'s modules directory at ${NTSHENGN_INSTALL_MODULE_PATH}.")
	set(CMAKE_INSTALL_PREFIX ${NTSHENGN_INSTALL_MODULE_PATH})
	if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
		install(TARGETS ${PROJECT_NAME}
			RUNTIME
			DESTINATION ${NTSHENGN_INSTALL_MODULE_PATH})
	else()
		install(TARGETS ${PROJECT_NAME}
			LIBRARY
			DESTINATION ${NTSHENGN_INSTALL_MODULE_PATH})
	endif()
elseif(NOT NTSHENGN_INSTALL_MODULE_PATH STREQUAL "NtshEngn_InstallModule_NOTFOUND")
	message(WARNING "Could not find NutshellEngine\'s modules directory at ${NTSHENGN_INSTALL_MODULE_PATH}.")
endif()