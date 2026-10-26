cmake_minimum_required(VERSION 3.21)

project(BurnoutAssist LANGUAGES CXX)

# BurnoutAssist only supports 32-bit Windows.
if(NOT WIN32)
    message(FATAL_ERROR "BurnoutAssist requires Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "BurnoutAssist must be built as 32-bit (x86)."
    )
endif()

# Change this if the source file has a different name
# or is located in a subdirectory.
set(BURNOUTASSIST_SOURCE "BurnoutAssist.cpp")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${BURNOUTASSIST_SOURCE}")
    message(FATAL_ERROR
        "Source file not found: ${BURNOUTASSIST_SOURCE}"
    )
endif()

# Build a Windows module, not an executable.
add_library(BurnoutAssist MODULE
    "${BURNOUTASSIST_SOURCE}"
)

# Use the required ASI output name.
set_target_properties(BurnoutAssist PROPERTIES
    PREFIX ""
    OUTPUT_NAME "BurnoutAssist"
    SUFFIX ".asi"

    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
