cmake_minimum_required(VERSION 3.16)
project(MI3DI VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /WX-)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

set(MI3DI_SOURCES
    src/driver.c
    src/log.c
    src/sf2.c
    src/sfz.c
    src/audio.c
)

find_package(OpenAL REQUIRED)

add_library(mi3di SHARED ${MI3DI_SOURCES} mi3di.def)

target_include_directories(mi3di PRIVATE
    include
    ${OPENAL_INCLUDE_DIR}
)

target_link_libraries(mi3di PRIVATE
    ${OPENAL_LIBRARY}
    winmm
)

target_compile_definitions(mi3di PRIVATE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

install(TARGETS mi3di RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES install.bat uninstall.bat README.md LICENSE DESTINATION .)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "MI3DI")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "MI3DI Project")
set(CPACK_PACKAGE_DESCRIPTION "MI3DI v${PROJECT_VERSION}")

if(NOT CPACK_PACKAGE_FILE_NAME)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CPACK_PACKAGE_FILE_NAME "MI3DI_v${PROJECT_VERSION}_x64")
    else()
        set(CPACK_PACKAGE_FILE_NAME "MI3DI_v${PROJECT_VERSION}_x86")
    endif()
endif()

include(CPack)
