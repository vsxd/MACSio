/*
Copyright (c) 2021, Xudong Sun.
Produced at EPCC, the University of Edinburgh.

All rights reserved.

Please also read the LICENSE file at the top of the source code directory or
folder hierarchy.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License (as published by the Free Software
Foundation) version 2, dated June 1991.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the terms and conditions of the GNU General
Public License for more details.
*/

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libs3.h>
#include <json-cwx/json.h>

#include <macsio_clargs.h>
#include <macsio_iface.h>
#include <macsio_log.h>
#include <macsio_main.h>
#include <macsio_mif.h>
#include <macsio_timing.h>
#include <macsio_utils.h>

#ifdef HAVE_MPI
#include <mpi.h>
#endif

#include <H5pubconf.h>
#include <hdf5.h>
/*!
\addtogroup plugins
@{
*/

/*!
\defgroup MACSIO_PLUGIN_HDF5 MACSIO_PLUGIN_HDF5
@{
*/

/*! \brief name of this plugin */
static char const *iface_name = "hdf5s3";

/*! \brief file extension for files managed by this plugin */
static char const *iface_ext = "h5";

static int use_log = 0;          /**< Use HDF5's logging fapl */
static int no_collective = 0;    /**< Use HDF5 independent (e.g. not collective) I/O */
static int no_single_chunk = 0;  /**< disable single chunking */
static int silo_block_size = 0;  /**< block size for silo block-based VFD */
static int silo_block_count = 0; /**< block count for silo block-based VFD */
static int sbuf_size = -1;       /**< HDF5 library sieve buf size */
static int mbuf_size = -1;       /**< HDF5 library meta blocck size */
static int rbuf_size = -1;       /**< HDF5 library small data block size */
static int lbuf_size = 0;        /**< HDF5 library log flags */
static const char *filename;
static hid_t fid;
static hid_t dspc = -1;
static int show_errors = 0;
static char compression_alg_str[64];
static char compression_params_str[512];

// A parameter for H5Pset_fapl_core()
static size_t vfd_core_increment = 1 << 21;

// All necessary configurations for the S3 API
static char *access_key = NULL;
static char *secret_key = NULL;
static char *host = NULL;
static char *auth_region = NULL;
static char *sample_bucket = NULL;
static int statusG = 0;
static char errorDetailsG[4096] = { 0 };

/*
libs3 related functions
*/
static void S3_init(void)
{ // init libs3
    S3Status status;
    
    if ((status = S3_initialize("s3", S3_INIT_ALL, host))
        != S3StatusOK) {
        fprintf(stderr, "Failed to initialize libs3: %s\n", 
                S3_get_status_name(status));
        exit(-1);
    }
}

static S3Status responsePropertiesCallback(
    const S3ResponseProperties *properties,
    void *callbackData)
{ // a callback function required by responseHandler
    return S3StatusOK;
}

// This callback does the same thing for every request type: saves the status
// and error stuff in global variables
static void responseCompleteCallback(
    S3Status status,
    const S3ErrorDetails *error,
    void *callbackData)
{
    statusG = status;
    int flag = 0;

    // Compose the error details message now, although we might not use it.
    // Can't just save a pointer to [error] since it's not guaranteed to last
    // beyond this callback
    int len = 0;
    if (error && error->message) {
        len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
                        "  Message: %s\n", error->message);
        printf("%s\n", errorDetailsG);
        flag = 1;
    }
    if (error && error->resource) {
        len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
                        "  Resource: %s\n", error->resource);
        printf("%s\n", errorDetailsG);
        flag = 1;
    }
    if (error && error->furtherDetails) {
        len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
                        "  Further Details: %s\n", error->furtherDetails);
        printf("%s\n", errorDetailsG);
        flag = 1;
    }
    if (error && error->extraDetailsCount) {
        len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
                        "%s", "  Extra Details:\n");
        int i;
        for (i = 0; i < error->extraDetailsCount; i++) {
            len += snprintf(&(errorDetailsG[len]), 
                            sizeof(errorDetailsG) - len, "    %s: %s\n", 
                            error->extraDetails[i].name,
                            error->extraDetails[i].value);
        }
        printf("%s\n", errorDetailsG);
        flag = 1;
    }
    if (flag || status != S3StatusOK) {
        // fast fail
        printf("responseCompleteCallback() got a error, exit.\n\n");
        exit(1);
    }
}

static const S3ResponseHandler responseHandler = {
    &responsePropertiesCallback,
    &responseCompleteCallback};

typedef struct put_object_callback_data
{
    const void * data;
    size_t contentLength; // Length of data (number of bytes)
} put_object_callback_data;

static int putObjectDataCallback(int bufferSize, char *buffer, void *callbackData)
{ // a callback function required by responseHandler
    put_object_callback_data *data = (put_object_callback_data *)callbackData;
    int ret = 0; // Number of bytes written in a single call

    if (data->contentLength)
    {
        if (data->contentLength >= bufferSize)
        { // Data length greater than buffer size
            memcpy(buffer, data->data, bufferSize);
            ret = bufferSize;
        }
        else
        { // Data can be entirely put into the buffer
            memcpy(buffer, data->data, data->contentLength);
            ret = data->contentLength;
        }
    }
    data->data = (char *) data->data + ret;
    data->contentLength -= ret;
    // printf("contentLength: %d\n", data->contentLength);
    return ret;
}


/*! \brief create HDF5 library file access property list */
static hid_t make_fapl()
{
    hid_t fapl_id = H5Pcreate(H5P_FILE_ACCESS);
    herr_t h5status = 0;

    /* Use Virtual File Drivers Core */
    herr_t ret_value = H5Pset_fapl_core(fapl_id, vfd_core_increment, false); //increment=2M
    if (ret_value < 0)
    {
        printf(">>>> hdf5 plugin, set Core VFD error");
    }

    if (sbuf_size >= 0)
        h5status |= H5Pset_sieve_buf_size(fapl_id, sbuf_size);

    if (mbuf_size >= 0)
        h5status |= H5Pset_meta_block_size(fapl_id, mbuf_size);

    if (rbuf_size >= 0)
        h5status |= H5Pset_small_data_block_size(fapl_id, mbuf_size);

    {
        H5AC_cache_config_t config;

        /* Acquire a default mdc config struct */
        config.version = H5AC__CURR_CACHE_CONFIG_VERSION;
        H5Pget_mdc_config(fapl_id, &config);
#define MAINZER_PARAMS 1
#if MAINZER_PARAMS
        config.set_initial_size = (hbool_t)1;
        config.initial_size = 16 * 1024;
        config.min_size = 8 * 1024;
        config.epoch_length = 3000;
        config.lower_hr_threshold = 0.95;
#endif
        H5Pset_mdc_config(fapl_id, &config);
    }

    if (h5status < 0)
    {
        if (fapl_id >= 0)
            H5Pclose(fapl_id);
        return 0;
    }

    return fapl_id;
}

/*!
\brief Utility to parse compression string command-line args

Does a case-insensitive search of \c src_str for \c token_to_match
not including the trailing format specifier. Upon finding a match, 
performs a scanf at the location of the match into temporary 
memory confirming the scan will actually succeed. Then, performs
the scanf again, storying the result to the memory indicated in
\c val_ptr.

\returns 0 on error, 1 on success
*/
static int
get_tokval(
    char const *src_str,        /**< CL arg string to be parsed */
    char const *token_to_match, /**< a token in the string to be matched including a trailing scanf format specifier */
    void *val_ptr               /**< Pointer to memory where parsed value should be placed */
)
{
    int toklen;
    char dummy[16];
    void *val_ptr_tmp = &dummy[0];

    if (!src_str)
        return 0;
    if (!token_to_match)
        return 0;

    toklen = strlen(token_to_match) - 2;

    if (strncasecmp(src_str, token_to_match, toklen))
        return 0;

    /* First, check matching sscanf *without* risk of writing to val_ptr */
    if (sscanf(&src_str[toklen], &token_to_match[toklen], val_ptr_tmp) != 1)
        return 0;

    sscanf(&src_str[toklen], &token_to_match[toklen], val_ptr);
    return 1;
}

/*!
\brief create HDF5 library dataset creation property list

If the dataset size is below the \c minsize threshold, no special
storage layout or compression action is taken.

Chunking is initially set to \em single-chunk. However, for szip
compressor, chunking can be set by command-line arguments.

*/
static hid_t
make_dcpl(
    char const *alg_str,    /**< compression algorithm string */
    char const *params_str, /**< compression params string */
    hid_t space_id,         /**< HDF5 dataspace id for the dataset */
    hid_t dtype_id          /**< HDF5 datatype id for the dataset */
)
{
    int shuffle = -1;
    int minsize = -1;
    int gzip_level = -1;
    int zfp_precision = -1;
    unsigned int szip_pixels_per_block = 0;
    float zfp_rate = -1;
    float zfp_accuracy = -1;
    char szip_method[64], szip_chunk_str[64];
    char *token, *string, *tofree;
    int ndims;
    hsize_t dims[4], maxdims[4];
    hid_t retval = H5Pcreate(H5P_DATASET_CREATE);

    szip_method[0] = '\0';
    szip_chunk_str[0] = '\0';

    /* Initially, set contiguous layout. May reset to chunked later */
    H5Pset_layout(retval, H5D_CONTIGUOUS);

    if (!alg_str || !strlen(alg_str))
        return retval;

    ndims = H5Sget_simple_extent_ndims(space_id);
    H5Sget_simple_extent_dims(space_id, dims, maxdims);

    /* We can make a pass through params string without being specific about
       algorithm because there are presently no symbol collisions there */
    tofree = string = strdup(params_str);
    while ((token = strsep(&string, ",")) != NULL)
    {
        if (get_tokval(token, "minsize=%d", &minsize))
            continue;
        if (get_tokval(token, "shuffle=%d", &shuffle))
            continue;
        if (get_tokval(token, "level=%d", &gzip_level))
            continue;
        if (get_tokval(token, "rate=%f", &zfp_rate))
            continue;
        if (get_tokval(token, "precision=%d", &zfp_precision))
            continue;
        if (get_tokval(token, "accuracy=%f", &zfp_accuracy))
            continue;
        if (get_tokval(token, "method=%s", szip_method))
            continue;
        if (get_tokval(token, "block=%u", &szip_pixels_per_block))
            continue;
        if (get_tokval(token, "chunk=%s", szip_chunk_str))
            continue;
    }
    free(tofree);

    /* check for minsize compression threshold */
    minsize = minsize != -1 ? minsize : 1024;
    if (H5Sget_simple_extent_npoints(space_id) < minsize)
        return retval;

    /*
     * Ok, now handle various properties related to compression
     */

    /* Initially, as a default in case nothing else is selected,
       set chunk size equal to dataset size (e.g. single chunk) */
    H5Pset_chunk(retval, ndims, dims);

    if (!strncasecmp(alg_str, "gzip", 4))
    {
        if (shuffle == -1 || shuffle == 1)
            H5Pset_shuffle(retval);
        H5Pset_deflate(retval, gzip_level != -1 ? gzip_level : 9);
    }
    else if (!strncasecmp(alg_str, "zfp", 3))
    {
        unsigned int cd_values[10];
        int cd_nelmts = 10;

        /* Add filter to the pipeline via generic interface */
        if (H5Pset_filter(retval, 32013, H5Z_FLAG_MANDATORY, cd_nelmts, cd_values) < 0)
            MACSIO_LOG_MSG(Warn, ("Unable to set up H5Z-ZFP compressor"));
    }
    else if (!strncasecmp(alg_str, "szip", 4))
    {
        static int have_issued_warning = 0;
        if (!have_issued_warning)
            MACSIO_LOG_MSG(Warn, ("szip compressor not available in this build"));
        have_issued_warning = 1;
    }

    return retval;
}

/*!
\brief Process command-line arguments an set local variables */
static int
process_args(
    int argi,    /**< argument index to start processing \c argv */
    int argc,    /**< \c argc from main */
    char *argv[] /**< \c argv from main */
)
{
    const MACSIO_CLARGS_ArgvFlags_t argFlags = {MACSIO_CLARGS_WARN, MACSIO_CLARGS_TOMEM};

    char *c_alg = compression_alg_str;
    char *c_params = compression_params_str;

    MACSIO_CLARGS_ProcessCmdline(0, argFlags, argi, argc, argv,
                                 "--show_errors", "",
                                 "Show low-level HDF5 errors",
                                 &show_errors,
                                 "--compression %s %s", MACSIO_CLARGS_NODEFAULT,
                                 "The first string argument is the compression algorithm name. The second\n"
                                 "string argument is a comma-separated set of params of the form\n"
                                 "'param1=val1,param2=val2,param3=val3. The various algorithm names and\n"
                                 "their parameter meanings are described below. Note that some parameters are\n"
                                 "not specific to any algorithm. Those are described first followed by\n"
                                 "individual algorithm-specific parameters for those algorithms available\n"
                                 "in the current build.\n"
                                 "\n"
                                 "minsize=%d : min. size of dataset (in terms of a count of values)\n"
                                 "    upon which compression will even be attempted. Default is 1024.\n"
                                 "shuffle=<int>: Boolean (zero or non-zero) to indicate whether to use\n"
                                 "    HDF5's byte shuffling filter *prior* to compression. Default depends\n"
                                 "    on algorithm. By default, shuffling is NOT used for zfp but IS\n"
                                 "    used with all other algorithms.\n"
                                 "\n"
                                 "Available compression algorithms...\n"
                                 "\n"
                                 "\"zfp\"\n"
                                 "    Use Peter Lindstrom's ZFP compression (computation.llnl.gov/casc/zfp)\n"
                                 "    Note: Whether this compression is available is determined entirely at\n"
                                 "    run-time using the H5Z-ZFP compresser as a generic filter. This means\n"
                                 "    all that is necessary is to specify the HDF5_PLUGIN_PATH environnment\n"
                                 "    variable with a path to the shared lib for the filter.\n"
                                 "    The following ZFP options are *mutually*exclusive*. In any command-line\n"
                                 "    specifying more than one of the following options, only the last\n"
                                 "    specified will be honored.\n"
                                 "        rate=%f : target # bits per compressed output datum. Fractional values\n"
                                 "            are permitted. 0 selects defaults: 4 bits/flt or 8 bits/dbl.\n"
                                 "            Use this option to hit a target compressed size but where error\n"
                                 "            varies. OTOH, use one of the following two options for fixed\n"
                                 "            error but amount of compression, if any, varies.\n"
                                 "        precision=%d : # bits of precision to preserve in each input datum.\n"
                                 "        accuracy=%f : absolute error tolerance in each output datum.\n"
                                 "            In many respects, 'precision' represents a sort of relative error\n"
                                 "            tolerance while 'accuracy' represents an absolute tolerance.\n"
                                 "            See http://en.wikipedia.org/wiki/Accuracy_and_precision.\n"
                                 "\n"
#ifdef HAVE_SZIP
                                 "\"szip\"\n"
                                 "    method=%s : specify 'ec' for entropy coding or 'nn' for nearest\n"
                                 "        neighbor. Default is 'nn'\n"
                                 "    block=%d : (pixels-per-block) must be an even integer <= 32. See\n"
                                 "        See H5Pset_szip in HDF5 documentation for more information.\n"
                                 "        Default is 32.\n"
                                 "    chunk=%d:%d : colon-separated dimensions specifying chunk size in\n"
                                 "        each dimension higher than the first (fastest varying) dimension.\n"
                                 "\n"
#endif
                                 "\"gzip\"\n"
                                 "    level=%d : A value in the range [1,9], inclusive, trading off time to\n"
                                 "        compress with amount of compression. Level=1 results in best speed\n"
                                 "        but worst compression whereas level=9 results in best compression\n"
                                 "        but worst speed. Values outside [1,9] are clamped. Default is 9.\n"
                                 "\n"
                                 "Examples:\n"
                                 "    --compression zfp rate=18.5\n"
                                 "    --compression gzip minsize=1024,level=9\n"
                                 "    --compression szip shuffle=0,options=nn,pixels_per_block=16\n"
                                 "\n",
                                 &c_alg, &c_params,
                                 "--no_collective", "",
                                 "Use independent, not collective, I/O calls in SIF mode.",
                                 &no_collective,
                                 "--no_single_chunk", "",
                                 "Do not single chunk the datasets (currently ignored).",
                                 &no_single_chunk,
                                 "--sieve_buf_size %d", MACSIO_CLARGS_NODEFAULT,
                                 "Specify sieve buffer size (see H5Pset_sieve_buf_size)",
                                 &sbuf_size,
                                 "--meta_block_size %d", MACSIO_CLARGS_NODEFAULT,
                                 "Specify size of meta data blocks (see H5Pset_meta_block_size)",
                                 &mbuf_size,
                                 "--small_block_size %d", MACSIO_CLARGS_NODEFAULT,
                                 "Specify threshold size for data blocks considered to be 'small'\n"
                                 "(see H5Pset_small_data_block_size)",
                                 &rbuf_size,
                                 "--log", "",
                                 "Use logging Virtual File Driver (see H5Pset_fapl_log)",
                                 &use_log,
#ifdef HAVE_SILO
                                 "--silo_fapl %d %d", MACSIO_CLARGS_NODEFAULT,
                                 "Use Silo's block-based VFD and specify block size and block count",
                                 &silo_block_size, &silo_block_count,
#endif
                                 MACSIO_CLARGS_END_OF_ARGS);

    if (!show_errors)
        H5Eset_auto1(0, 0);
    return 0;
}

/*! \brief User data for MIF callbacks */
typedef struct _user_data
{
    hid_t groupId; /**< HDF5 hid_t of current group */
} user_data_t;

/*! \brief MIF create file callback for HDF5 MIF mode */
static void *
CreateHDF5File(
    const char *fname,  /**< file name */
    const char *nsname, /**< curent task namespace name */
    void *userData      /**< user data specific to current task */
)
{
    hid_t *retval = 0;
    hid_t h5File;
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);

    /* Use Virtual File Drivers Core */
    herr_t ret_value = H5Pset_fapl_core(fapl, vfd_core_increment, false); //increment=2M
    if (ret_value < 0)
    {
        printf(">>>> hdf5 plugin, set Core VFD error");
    }

    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    h5File = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    H5Pclose(fapl);
    if (h5File >= 0)
    {
        //#warning USE NEWER GROUP CREATION SETTINGS OF HDF5
        if (nsname && userData)
        {
            user_data_t *ud = (user_data_t *)userData;
            ud->groupId = H5Gcreate1(h5File, nsname, 0);
        }
        retval = (hid_t *)malloc(sizeof(hid_t));
        *retval = h5File;
    }
    return (void *)retval;
}

/*! \brief MIF Open file callback for HFD5 plugin MIF mode */
static void *
OpenHDF5File(
    const char *fname,            /**< filename */
    const char *nsname,           /**< namespace name for current task */
    MACSIO_MIF_ioFlags_t ioFlags, /* io flags */
    void *userData                /**< task specific user data for current task */
)
{
    hid_t *retval;
    hid_t h5File;
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);

    /* Use Virtual File Drivers Core */
    herr_t ret_value = H5Pset_fapl_core(fapl, vfd_core_increment, false); //increment=2M
    if (ret_value < 0)
    {
        printf(">>>> hdf5 plugin, set Core VFD error");
    }

    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    h5File = H5Fopen(fname, ioFlags.do_wr ? H5F_ACC_RDWR : H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    if (h5File >= 0)
    {
        if (ioFlags.do_wr && nsname && userData)
        {
            user_data_t *ud = (user_data_t *)userData;
            ud->groupId = H5Gcreate1(h5File, nsname, 0);
        }
        retval = (hid_t *)malloc(sizeof(hid_t));
        *retval = h5File;
    }
    return (void *)retval;
}

/*! \brief MIF close file callback for HDF5 plugin MIF mode */
static int
CloseHDF5File(
    void *file,    /**< void* to hid_t of file to cose */
    void *userData /**< task specific user data */
)
{
    const unsigned int obj_flags = H5F_OBJ_LOCAL | H5F_OBJ_DATASET |
                                   H5F_OBJ_GROUP | H5F_OBJ_DATATYPE | H5F_OBJ_ATTR;
    int noo;
    herr_t close_retval;

    if (userData)
    {
        user_data_t *ud = (user_data_t *)userData;
        if (H5Iis_valid(ud->groupId) > 0 && H5Iget_type(ud->groupId) == H5I_GROUP)
            H5Gclose(ud->groupId);
    }

    /* Check for any open objects in this file */
    if (fid == (hid_t)H5F_OBJ_ALL ||
        (H5Iis_valid(fid) > 0) && H5Iget_type(fid) == H5I_FILE)
        noo = H5Fget_obj_count(fid, obj_flags);
    close_retval = H5Fclose(*((hid_t *)file));
    free(file);

    if (noo > 0)
        return -1;
    return (int)close_retval;
}

/*! \brief Write individual mesh part in MIF mode */
static void
write_mesh_part(
    hid_t h5loc,          /**< HDF5 group id into which to write */
    json_object *part_obj /**< JSON object for the mesh part to write */
)
{
    //#warning WERE SKPPING THE MESH (COORDS) OBJECT PRESENTLY
    int i;
    json_object *vars_array = json_object_path_get_array(part_obj, "Vars");

    for (i = 0; i < json_object_array_length(vars_array); i++)
    {
        int j;
        hsize_t var_dims[3];
        hid_t fspace_id, ds_id, dcpl_id;
        json_object *var_obj = json_object_array_get_idx(vars_array, i);
        json_object *data_obj = json_object_path_get_extarr(var_obj, "data");
        char const *varname = json_object_path_get_string(var_obj, "name");
        int ndims = json_object_extarr_ndims(data_obj);
        void const *buf = json_object_extarr_data(data_obj);
        hid_t dtype_id = json_object_extarr_type(data_obj) == json_extarr_type_flt64 ? H5T_NATIVE_DOUBLE : H5T_NATIVE_INT;

        for (j = 0; j < ndims; j++)
            var_dims[j] = json_object_extarr_dim(data_obj, j);

        fspace_id = H5Screate_simple(ndims, var_dims, 0);
        dcpl_id = make_dcpl(compression_alg_str, compression_params_str, fspace_id, dtype_id);
        ds_id = H5Dcreate1(h5loc, varname, dtype_id, fspace_id, dcpl_id);
        H5Dwrite(ds_id, dtype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
        H5Dclose(ds_id);
        H5Pclose(dcpl_id);
        H5Sclose(fspace_id);
    }
}

/*! \brief Main dump output for HDF5 plugin MIF mode */
static void
main_dump_mif(
    json_object *main_obj, /**< main data object to dump */
    int numFiles,          /**< MIF file count */
    int dumpn,             /**< dump number (like a cycle number) */
    double dumpt           /**< dump time */
)
{
    MACSIO_TIMING_GroupMask_t main_dump_mif_grp = MACSIO_TIMING_GroupMask("main_dump_mif");
    MACSIO_TIMING_TimerId_t main_dump_mif_tid;
    double timer_dt;

    int size, rank;
    hid_t *h5File_ptr;
    hid_t h5File;
    hid_t h5Group;
    char fileName[256];
    int i, len;
    int *theData;
    user_data_t userData;
    MACSIO_MIF_ioFlags_t ioFlags = {MACSIO_MIF_WRITE,
                                    (unsigned)JsonGetInt(main_obj, "clargs/exercise_scr") & 0x1};

    //#warning MAKE WHOLE FILE USE HDF5 1.8 INTERFACE
    //#warning SET FILE AND DATASET PROPERTIES
    //#warning DIFFERENT MPI TAGS FOR DIFFERENT PLUGINS AND CONTEXTS
    main_dump_mif_tid = MT_StartTimer("MACSIO_MIF_INIT", main_dump_mif_grp, dumpn);
    MACSIO_MIF_baton_t *bat = MACSIO_MIF_Init(numFiles, ioFlags, MACSIO_MAIN_Comm, 3,
                                              CreateHDF5File, OpenHDF5File, CloseHDF5File, &userData);
    timer_dt = MT_StopTimer(main_dump_mif_tid);

    rank = json_object_path_get_int(main_obj, "parallel/mpi_rank");
    size = json_object_path_get_int(main_obj, "parallel/mpi_size");

    /* Construct name for the silo file */
    sprintf(fileName, "%s_hdf5s3_%05d_%03d.%s",
            json_object_path_get_string(main_obj, "clargs/filebase"),
            MACSIO_MIF_RankOfGroup(bat, rank),
            dumpn,
            json_object_path_get_string(main_obj, "clargs/fileext"));

    MACSIO_UTILS_RecordOutputFiles(dumpn, fileName);

    h5File_ptr = (hid_t *)MACSIO_MIF_WaitForBaton(bat, fileName, 0);
    h5File = *h5File_ptr;
    h5Group = userData.groupId;

    json_object *parts = json_object_path_get_array(main_obj, "problem/parts");

    for (int i = 0; i < json_object_array_length(parts); i++)
    {
        char domain_dir[256];
        json_object *this_part = json_object_array_get_idx(parts, i);
        hid_t domain_group_id;

        snprintf(domain_dir, sizeof(domain_dir), "domain_%07d",
                 json_object_path_get_int(this_part, "Mesh/ChunkID"));

        domain_group_id = H5Gcreate1(h5File, domain_dir, 0);

        main_dump_mif_tid = MT_StartTimer("write_mesh_part", main_dump_mif_grp, dumpn);
        write_mesh_part(domain_group_id, this_part);
        timer_dt = MT_StopTimer(main_dump_mif_tid);

        H5Gclose(domain_group_id);
    }

    put_object_callback_data put_data;

    H5Fflush(h5File, H5F_SCOPE_GLOBAL);
    /* get the size of the file */
    size_t image_size = H5Fget_file_image(h5File, NULL, (size_t)0);
    put_data.contentLength = image_size;
    /* allocate a buffer of the appropriate size */
    void *image_ptr = malloc((size_t)image_size);
    /* load the image of the file into the buffer */
    size_t bytes_read = H5Fget_file_image(h5File, image_ptr, (size_t)image_size);
    put_data.data = image_ptr;

    /* Debug output */
    // printf("image_size: %d\n", image_size);
    // printf("bytes_read: %d\n", bytes_read);
    // printf("%s\n", (char *)image_ptr);
    S3_init();

    const S3PutObjectHandler putObjectHandler =
    {
        responseHandler,
        &putObjectDataCallback};
    
    const S3BucketContext bucketContext = {
        host,
        sample_bucket,
        S3ProtocolHTTP,
        S3UriStylePath,
        access_key,
        secret_key,
        NULL,
        auth_region};

    /* Put h5 file to object store */
    main_dump_mif_grp = MT_StartTimer("write_s3_mif", main_dump_mif_grp, dumpn);
    S3_put_object(&bucketContext, fileName, image_size, NULL, NULL, 0, &putObjectHandler, &put_data);
    timer_dt = MT_StopTimer(main_dump_mif_tid);

    /* For debug, write h5 file to disk */
    // printf("S3 put object: %s\n", fileName);
    // FILE *file = fopen(fileName, "wb");
    // fwrite(image_ptr, image_size, 1, file);
    // fclose(file);

    /* Hand off the baton to the next processor. This winds up closing
     * the file so that the next processor that opens it can be assured
     * of getting a consistent and up to date view of the file's contents. */
    main_dump_mif_tid = MT_StartTimer("MACSIO_MIF_HandOffBaton", main_dump_mif_grp, dumpn);
    MACSIO_MIF_HandOffBaton(bat, h5File_ptr);
    timer_dt = MT_StopTimer(main_dump_mif_tid);

    /* We're done using MACSIO_MIF, so finish it off */
    main_dump_mif_tid = MT_StartTimer("MACSIO_MIF_Finish", main_dump_mif_grp, dumpn);
    MACSIO_MIF_Finish(bat);
    timer_dt = MT_StopTimer(main_dump_mif_tid);
    
    S3_deinitialize();
}

/*!
\brief Main dump callback for HDF5 plugin

Selects between MIF and SSF output.
*/
static void
main_dump(
    int argi,              /**< arg index at which to start processing \c argv */
    int argc,              /**< \c argc from main */
    char **argv,           /**< \c argv from main */
    json_object *main_obj, /**< main json data object to dump */
    int dumpn,             /**< dump number */
    double dumpt           /**< dump time */
)
{
    MACSIO_TIMING_GroupMask_t main_dump_grp = MACSIO_TIMING_GroupMask("main_dump");
    MACSIO_TIMING_TimerId_t main_dump_tid;
    double timer_dt;

    int rank, size, numFiles;

    //#warning SET ERROR MODE OF HDF5 LIBRARY

    /* Without this barrier, I get strange behavior with Silo's MACSIO_MIF interface */
#ifdef HAVE_MPI
    mpi_errno = MPI_Barrier(MACSIO_MAIN_Comm);
#endif

    /* process cl args */
    process_args(argi, argc, argv);

    rank = json_object_path_get_int(main_obj, "parallel/mpi_rank");
    size = json_object_path_get_int(main_obj, "parallel/mpi_size");

    //#warning MOVE TO A FUNCTION
    /* ensure we're in MIF mode and determine the file count */
    json_object *parfmode_obj = json_object_path_get_array(main_obj, "clargs/parallel_file_mode");
    if (parfmode_obj)
    {
        json_object *modestr = json_object_array_get_idx(parfmode_obj, 0);
        json_object *filecnt = json_object_array_get_idx(parfmode_obj, 1);
        //#warning ERRORS NEED TO GO TO LOG FILES AND ERROR BEHAVIOR NEEDS TO BE HONORED
        if (!strcmp(json_object_get_string(modestr), "SIF"))
        {
            // main_dump_tid = MT_StartTimer("main_dump_sif", main_dump_grp, dumpn);
            MACSIO_LOG_MSG(Die, ("HDF5_S3 plugin cannot currently handle SIF mode"));
            // timer_dt = MT_StopTimer(main_dump_tid);
        }
        else
        {
            numFiles = json_object_get_int(filecnt);
            main_dump_tid = MT_StartTimer("main_dump_mif", main_dump_grp, dumpn);
            main_dump_mif(main_obj, numFiles, dumpn, dumpt);
            timer_dt = MT_StopTimer(main_dump_tid);
        }
    }
    else
    {
        char const *modestr = json_object_path_get_string(main_obj, "clargs/parallel_file_mode");
        if (!strcmp(modestr, "SIF"))
        {
            MACSIO_LOG_MSG(Die, ("HDF5_S3 plugin cannot currently handle SIF mode"));
        }
        else if (!strcmp(modestr, "MIFMAX"))
            numFiles = json_object_path_get_int(main_obj, "parallel/mpi_size");
        else if (!strcmp(modestr, "MIFAUTO"))
        {
            /* Call utility to determine optimal file count */
            //#warning ADD UTILIT TO DETERMINE OPTIMAL FILE COUNT
        }
        main_dump_tid = MT_StartTimer("main_dump_mif", main_dump_grp, dumpn);
        main_dump_mif(main_obj, numFiles, dumpn, dumpt);
        timer_dt = MT_StopTimer(main_dump_tid);
    }
}

/*! \brief Function called during static initialization to register the plugin */
static int
register_this_interface()
{
    MACSIO_IFACE_Handle_t iface;

    if (strlen(iface_name) >= MACSIO_IFACE_MAX_NAME)
        MACSIO_LOG_MSG(Die, ("Interface name \"%s\" too long", iface_name));

    //#warning DO HDF5 LIB WIDE (DEFAULT) INITITILIAZATIONS HERE

    /* Populate information about this plugin */
    strcpy(iface.name, iface_name);
    strcpy(iface.ext, iface_ext);
    iface.dumpFunc = main_dump;
    iface.processArgsFunc = process_args;

    /* Register custom compression methods with HDF5 library */
    H5dont_atexit();

    /* Register this plugin */
    if (!MACSIO_IFACE_Register(&iface))
        MACSIO_LOG_MSG(Die, ("Failed to register interface \"%s\"", iface_name));

    // Read environment variables for S3 configurations
    access_key = getenv("S3_ACCESS_KEY");
    secret_key = getenv("S3_SECRET_KEY");
    host = getenv("S3_HOST");
    char *p = getenv("S3_REGION");
    if (p != NULL) auth_region = p;
    sample_bucket = getenv("S3_BUCKET");

    // for debug
    // printf("==== S3 info\nhost=%s\nauth_region=%s\naccess_key=%s\nsecret_key=%s\nsample_bucket=%s\n====\n", host, auth_region, access_key, secret_key, sample_bucket);

    return 0;
}

/*! \brief Static initializer statement to cause plugin registration at link time

this one statement is the only statement requiring compilation by
a C++ compiler. That is because it involves initialization and non
constant expressions (a function call in this case). This function
call is guaranteed to occur during *initialization* (that is before
even 'main' is called) and so will have the effect of populating the
iface_map array merely by virtue of the fact that this code is linked
with a main.
*/
static int dummy = register_this_interface();

/*!@}*/

/*!@}*/
