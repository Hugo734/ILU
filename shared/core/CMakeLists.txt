# ESP_PLATFORM lo define ESP-IDF. Dentro de un build de IDF esto es un
# componente; fuera, una libreria normal que compila con g++.
set(CORE_SRCS protocol.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${CORE_SRCS} INCLUDE_DIRS ".")
else()
    add_library(core STATIC ${CORE_SRCS})
    target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(core PUBLIC cxx_std_17)
    target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic)
endif()
