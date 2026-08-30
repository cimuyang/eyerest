cmake_minimum_required(VERSION 3.21)

project(EyeRest LANGUAGES CXX RC)

add_executable(EyeRest WIN32
    src/main.cpp
    resources/EyeRest.rc
)

target_compile_features(EyeRest PRIVATE cxx_std_20)
target_compile_definitions(EyeRest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(EyeRest PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(EyeRest PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_compile_options(EyeRest PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(EyeRest PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(EyeRest PRIVATE
    comctl32
    shell32
    shlwapi
    advapi32
    wtsapi32
)

install(TARGETS EyeRest RUNTIME DESTINATION .)
