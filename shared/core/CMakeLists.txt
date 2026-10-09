# ESP_PLATFORM is defined by ESP-IDF. Inside an IDF build this is a
# component; outside it, a plain library that compiles with g++.
set(CORE_SRCS protocol.cpp command.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${CORE_SRCS} INCLUDE_DIRS ".")
else()
    add_library(core STATIC ${CORE_SRCS})
    target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(core PUBLIC cxx_std_17)
    target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic)
endif()
