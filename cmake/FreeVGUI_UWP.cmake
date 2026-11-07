# FreeVGUI for Xbox Series Dev Mode (UWP) builds.
#
# freevgui/CMakeLists.txt picks the Win32 platform backend on any WIN32 target,
# and its font code rasterises through GDI, which app containers don't have.
# Build the same library here with the Win32 app backend (timing only) and the
# portable font stub the POSIX port uses, so the client falls back to the
# engine's own fonts.

set(FREEVGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/freevgui)

file(GLOB FREEVGUI_SOURCES
	${FREEVGUI_DIR}/*.cpp
	${FREEVGUI_DIR}/controls/*.cpp
	${FREEVGUI_DIR}/platform/common/*.cpp
	${FREEVGUI_DIR}/platform/win32/app.cpp
	${FREEVGUI_DIR}/platform/posix/font.cpp)

if(USE_STATIC_FREEVGUI)
	file(GLOB FREEVGUI_XASH_SOURCES ${FREEVGUI_DIR}/platform/xash3d-fwgs/*.cpp)
	list(APPEND FREEVGUI_SOURCES ${FREEVGUI_XASH_SOURCES})
	add_library(vgui STATIC ${FREEVGUI_SOURCES})
	target_include_directories(vgui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)
	target_compile_definitions(vgui PRIVATE INTERNAL_VGUI_SUPPORT)
else()
	add_library(vgui SHARED ${FREEVGUI_SOURCES})
endif()

target_include_directories(vgui PRIVATE ${FREEVGUI_DIR})
target_compile_features(vgui PUBLIC cxx_std_11)
set_target_properties(vgui PROPERTIES POSITION_INDEPENDENT_CODE 1)
