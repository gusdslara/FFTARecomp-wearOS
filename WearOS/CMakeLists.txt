cmake_minimum_required(VERSION 3.16)
project(ffta C)
if(WIN32)
	enable_language(RC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- SDL3
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
if(ANDROID)
	# SDLActivity loads libSDL3.so + libmain.so
	set(SDL_SHARED ON CACHE BOOL "" FORCE)
	set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(externals/SDL EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- mGBA (libmgba only)
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(M_CORE_GB OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(ENABLE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(ENABLE_GDB_STUB OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_PNG OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP OFF CACHE BOOL "" FORCE)
set(USE_LZMA OFF CACHE BOOL "" FORCE)
set(USE_ELF OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_LUA OFF CACHE BOOL "" FORCE)
set(USE_JSON_C OFF CACHE BOOL "" FORCE)
set(USE_FFMPEG OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
set(USE_EPOXY OFF CACHE BOOL "" FORCE)
set(BUILD_GL OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2 OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3 OFF CACHE BOOL "" FORCE)
set(LIBMGBA_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(externals/mgba EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- game shell
set(FFTA_SRC
	src/main.c
	src/emu.c
	src/input.c
	src/view.c
)

if(ANDROID)
	add_library(main SHARED ${FFTA_SRC})
	set(FFTA_TARGET main)
	target_link_libraries(main PRIVATE android log)
else()
	if(WIN32)
		list(APPEND FFTA_SRC res/ffta.rc)  # .exe icon
	endif()
	add_executable(ffta ${FFTA_SRC})
	set(FFTA_TARGET ffta)
	if(EMSCRIPTEN)
		set_target_properties(ffta PROPERTIES SUFFIX ".html")
		target_link_options(ffta PRIVATE
			"SHELL:-s ALLOW_MEMORY_GROWTH=1"
			"SHELL:-s FORCE_FILESYSTEM=1"
			"SHELL:-s EXPORTED_RUNTIME_METHODS=['FS']"
			"SHELL:-lidbfs.js"
			"--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html")
	endif()
endif()

target_link_libraries(${FFTA_TARGET} PRIVATE SDL3::SDL3 mgba)
if(MINGW)
	target_link_options(${FFTA_TARGET} PRIVATE -static -static-libgcc)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(${FFTA_TARGET} PRIVATE -Wall -Wno-unused-parameter)
endif()
