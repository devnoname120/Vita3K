add_library(
	sdl-frontend
	STATIC
	include/sdl-frontend/session.h
	src/frame_host.cpp
	src/frame_host.h
	src/session.cpp
)

target_include_directories(sdl-frontend PUBLIC include)
target_link_libraries(sdl-frontend PUBLIC emuenv)
target_link_libraries(sdl-frontend PRIVATE app audio config ctrl dialog ime motion renderer touch util SDL3::SDL3)

if(NOT ANDROID)
	find_package(Python3 QUIET COMPONENTS Interpreter)
	if(Python3_Interpreter_FOUND)
		add_test(
			NAME sdl-frontend-mouse-forwarding
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/tests/mouse_forwarding_test.py
				${CMAKE_CURRENT_SOURCE_DIR}/src/session.cpp
		)
	endif()
endif()
