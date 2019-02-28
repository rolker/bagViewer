cmake_minimum_required(VERSION 3.5.1)

project(bagViewer)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt5 COMPONENTS Widgets OpenGL)

if (Qt5Widgets_FOUND)
    if (Qt5Widgets_VERSION VERSION_LESS 5.4.0)
        message(FATAL_ERROR "Minimum supported Qt5 version is 5.4!")
    endif()
else()
    message(SEND_ERROR "The Qt5Widgets library could not be found!")
endif(Qt5Widgets_FOUND)

find_package(OpenGL)

set (SHADERS vertex.glsl tes.glsl geometry.glsl fragment.glsl)

set ( RESOURCES
 resources.qrc
)

find_path(BAG_INCLUDE_DIR NAMES bag.h)
find_library(BAG_LIBRARY bag)
include_directories(${BAG_INCLUDE_DIR})

if(CMAKE_COMPILER_IS_GNUCXX)
    add_definitions(-std=c++11)
endif(CMAKE_COMPILER_IS_GNUCXX)

set( HDR BagViewer.h BagGL.h)
set( SRC main.cpp BagViewer.cpp BagGL.cpp BagIO.cpp)
if( MSVC )
	set( SRC ${SRC} BagViewer.rc )
endif()
add_executable(bagViewer ${HDR} ${SRC} ${RESOURCES})

qt5_use_modules(bagViewer Widgets OpenGL)
target_link_libraries(bagViewer ${OPENGL_gl_LIBRARY} ${BAG_LIBRARY} ${QT_LIBRARIES})

INSTALL(TARGETS bagViewer RUNTIME DESTINATION bin)
