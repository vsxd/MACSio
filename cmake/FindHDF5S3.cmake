# - Try to find libhdf5
# Once done this will define
#  HDF5_FOUND - System has libhdf5
#  HDF5_INCLUDE_DIRS - The libhdf5 include directories
#  HDF5_LIBRARIES - The libraries needed to use libhdf5

FIND_PATH(WITH_HDF5_PREFIX
    NAMES include/hdf5.h
)

FIND_LIBRARY(HDF5_LIBRARIES
    NAMES hdf5
    HINTS ${WITH_HDF5_PREFIX}/lib
)

FIND_PATH(HDF5_INCLUDE_DIRS
    NAMES hdf5.h
    HINTS ${WITH_HDF5_PREFIX}/include
)

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
FIND_PACKAGE_HANDLE_STANDARD_ARGS(HDF5 DEFAULT_MSG
    HDF5_LIBRARIES
    HDF5_INCLUDE_DIRS
    LIBS3_LIBRARIES
    LIBS3_INCLUDE_DIRS
)

# Hide these vars from ccmake GUI
MARK_AS_ADVANCED(
	HDF5_LIBRARIES
	HDF5_INCLUDE_DIRS
    LIBS3_LIBRARIES
    LIBS3_INCLUDE_DIRS
)
