# - Try to find libs3
# Once done this will define
#  LIBS3_FOUND - System has libs3
#  LIBS3_INCLUDE_DIRS - The libs3 include directories
#  LIBS3_LIBRARIES - The libraries needed to use libs3

FIND_PATH(WITH_LIBS3_PREFIX
        NAMES include/libs3.h
        )

FIND_LIBRARY(LIBS3_LIBRARIES
        NAMES libs3.so
        HINTS ${WITH_LIBS3_PREFIX}/lib
        )

FIND_PATH(LIBS3_INCLUDE_DIRS
        NAMES libs3.h
        HINTS ${WITH_LIBS3_PREFIX}/include
        )

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBS3 DEFAULT_MSG
        LIBS3_LIBRARIES
        LIBS3_INCLUDE_DIRS
        )

# Hide these vars from ccmake GUI
MARK_AS_ADVANCED(
        LIBS3_LIBRARIES
        LIBS3_INCLUDE_DIRS
)