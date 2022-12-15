cmake_minimum_required(VERSION 3.1 FATAL_ERROR)
set(NAME GLAD)
project(${NAME} VERSION 0.0.1 LANGUAGES C CXX)

set(GLAD_SOURCES
	src/glad.c)
set(GLAD_HEADERS
	include/glad/glad.h)

add_library(${PROJECT_NAME} ${GLAD_SOURCES} ${GLAD_HEADERS})

target_include_directories(${PROJECT_NAME}
	PUBLIC
	include)