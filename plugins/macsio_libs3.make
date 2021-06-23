
# This floating point variable is used to order plugin objects during
# the main link for MACSio to allow dependent libraries that are common
# to multiple plugins to be placed later on the link line. Larger 
# numbers here cause them to appear later in the link line relative to
# smaller numbers
LIBS3_BUILD_ORDER = 2.0

# Convenience variable to simplify paths to header and library/archive files
# for the I/O library being used by this plugin
LIBS3_HOME =

# Convenience variables for any dependent libs (not always needed) for the
# I/O library being used by this plugin
FOO_LIB = /usr/local/foo
BAR_LIB = /etc/bar

# Compiler flags for this plugin to find the I/O library's header files
#MIFTMPL_CFLAGS = -I$(FOOIF_HOME)/include
LIBS3_CFLAGS =

# Linker flags for this plugin to find the I/O library's lib/archive files
#MIFTMPL_LDFLAGS = -L$(FOOIF_HOME)/lib -L$(FOO_LIB) -L$(BAR_LIB) -lfooif -lfoo -lbar
LIBS3_LDFLAGS = -lcurl -lxml2 -lcrypto -lssl

# List of source files used by this plugin (usually just one)
LIBS3_SOURCES = macsio_libs3.c

# Main Makefile variables that this plugin updates
PLUGIN_OBJECTS += $(LIBS3_SOURCES:.c=.o)
PLUGIN_LDFLAGS += $(LIBS3_LDFLAGS)
PLUGIN_LIST += libs3

# Rules to build the object file(s) for this plugin
macsio_libs3.o: ../plugins/macsio_libs3.c
	$(CXX) -c $(LIBS3_CFLAGS) $(MACSIO_CFLAGS) $(CFLAGS) ../plugins/macsio_libs3.c
