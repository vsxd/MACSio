# Copyright (c) 2021, Xudong Sun.
# Produced at EPCC, the University of Edinburgh.
#
# All rights reserved.
# 
# Please also read the LICENSE file at the top of the source code directory or
# folder hierarchy.
# 
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License (as published by the Free Software
# Foundation) version 2, dated June 1991.
# 
# This program is distributed in the hope that it will be useful, but WITHOUT 
# ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the terms and conditions of the GNU General
# Public License for more details.

# This floating point variable is used to order plugin objects during
# the main link for MACSio to allow dependent libraries that are common
# to multiple plugins to be placed later on the link line. Bigger
# numbers here cause them to appear later in the link line.
HDF5S3_BUILD_ORDER = 1.0

HDF5S3_VERSION = 0.1

ifneq ($(HDF5_HOME),)

HDF5S3_LDFLAGS = -L$(HDF5_HOME)/lib -lhdf5 -Wl,-rpath,$(HDF5_HOME)/lib
HDF5S3_CFLAGS = -I$(HDF5_HOME)/include

HDF5S3_SOURCES = macsio_hdf5s3.c

HDF5S3_LDFLAGS += -lz -lm

PLUGIN_OBJECTS += $(HDF5S3_SOURCES:.c=.o)
PLUGIN_LDFLAGS += $(HDF5S3_LDFLAGS)
PLUGIN_LIST += hdf5s3

endif

macsio_hdf5s3.o: ../plugins/macsio_hdf5s3.c
	$(CXX) -c $(HDF5S3_CFLAGS) $(MACSIO_CFLAGS) $(CFLAGS) ../plugins/macsio_hdf5s3.c
