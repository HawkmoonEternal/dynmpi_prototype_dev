/*
 * Copyright (c) 2013      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#define _GNU_SOURCE
#include <stdio.h>

#include <sys/types.h>
#include <unistd.h>

#include "oshmem_config.h"
#include "orte/util/show_help.h"
#include "shmem.h"
#include "oshmem/runtime/params.h"
#include "oshmem/mca/spml/spml.h"
#include "oshmem/mca/spml/base/base.h"
#include "spml_ikrit_component.h"
#include "oshmem/mca/spml/ikrit/spml_ikrit.h"

#include "orte/util/show_help.h"

static int mca_spml_ikrit_component_register(void);
static int mca_spml_ikrit_component_open(void);
static int mca_spml_ikrit_component_close(void);
static mca_spml_base_module_t*
mca_spml_ikrit_component_init(int* priority,
                              bool enable_progress_threads,
                              bool enable_mpi_threads);
static int mca_spml_ikrit_component_fini(void);
mca_spml_base_component_2_0_0_t mca_spml_ikrit_component = {

    /* First, the mca_base_component_t struct containing meta
       information about the component itself */

    {
      MCA_SPML_BASE_VERSION_2_0_0,
    
      "ikrit",                        /* MCA component name */
      OSHMEM_MAJOR_VERSION,           /* MCA component major version */
      OSHMEM_MINOR_VERSION,           /* MCA component minor version */
      OSHMEM_RELEASE_VERSION,         /* MCA component release version */
      mca_spml_ikrit_component_open,  /* component open */
      mca_spml_ikrit_component_close, /* component close */
      NULL,
      mca_spml_ikrit_component_register
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    mca_spml_ikrit_component_init,    /* component init */
    mca_spml_ikrit_component_fini     /* component finalize */
    
};

static inline int mca_spml_ikrit_param_register_int(const char* param_name,
                                                    int default_value,
                                                    const char *help_msg)
{
    int param_value;

    param_value = default_value;
    (void) mca_base_component_var_register(&mca_spml_ikrit_component.spmlm_version,
                                           param_name,
                                           help_msg,
                                           MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           OPAL_INFO_LVL_9,
                                           MCA_BASE_VAR_SCOPE_READONLY,
                                           &param_value);

    return param_value;
}

static void  mca_spml_ikrit_param_register_string(const char* param_name,
                                                    char* default_value,
                                                    const char *help_msg,
                                                    char **storage)
{
    *storage = default_value;
    (void) mca_base_component_var_register(&mca_spml_ikrit_component.spmlm_version,
                                           param_name,
                                           help_msg,
                                           MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                           OPAL_INFO_LVL_9,
                                           MCA_BASE_VAR_SCOPE_READONLY,
                                           storage);

}

static int mca_spml_ikrit_component_register(void)
{
    int np;

    mca_spml_ikrit.free_list_num =
            mca_spml_ikrit_param_register_int("free_list_num", 1024, 0);
    mca_spml_ikrit.free_list_max =
            mca_spml_ikrit_param_register_int("free_list_max", 1024, 0);
    mca_spml_ikrit.free_list_inc =
            mca_spml_ikrit_param_register_int("free_list_inc", 16, 0);
    mca_spml_ikrit.priority =
            mca_spml_ikrit_param_register_int("priority",
                                              20,
                                              "[integer] ikrit priority");

    mca_spml_ikrit_param_register_string("mxm_tls",
                                         "ud,self",
                                         "[string] TL channels for MXM",
                                         &mca_spml_ikrit.mxm_tls);

    mca_spml_ikrit.n_relays =
            mca_spml_ikrit_param_register_int("use_relays",
                                              -1,
                                              "[integer] First N ranks on host will receive and forward put messages to other ranks running on it. Can be used to as work around Sandy Bridge far socket problem");

    np = mca_spml_ikrit_param_register_int("np",
#if MXM_API <= MXM_VERSION(2,0)
                                           128,
#else
                                           0,
#endif
                                           "[integer] Minimal allowed job's NP to activate ikrit");
    if (oshmem_num_procs() < np) {
        SPML_VERBOSE(1,
                     "Not enough ranks (%d<%d), disqualifying spml/ikrit",
                     oshmem_num_procs(), np);
        return OSHMEM_ERR_NOT_AVAILABLE;
    }

    return OSHMEM_SUCCESS;
}

int spml_ikrit_progress(void)
{
    mxm_error_t err;

    err = mxm_progress(mca_spml_ikrit.mxm_context);
    if ((MXM_OK != err) && (MXM_ERR_NO_PROGRESS != err)) {
        orte_show_help("help-shmem-spml-ikrit.txt",
                       "errors during mxm_progress",
                       true,
                       mxm_error_string(err));
    }
    return 1;
}

static int mca_spml_ikrit_component_open(void)
{
    mxm_error_t err;
    unsigned long cur_ver;

    cur_ver = mxm_get_version();
    if (cur_ver != MXM_API) {
        char *str;
        if (asprintf(&str,
                     "SHMEM was compiled with MXM version %d.%d but "
                     "version %ld.%ld detected.",
                     MXM_VERNO_MAJOR,
                     MXM_VERNO_MINOR,
                     (cur_ver >> MXM_MAJOR_BIT) & 0xff,
                     (cur_ver >> MXM_MINOR_BIT) & 0xff) > 0) {
            orte_show_help("help-shmem-spml-ikrit.txt", "mxm init", true, str);

            free(str);
        }
        return OSHMEM_ERROR;
    }

#if MXM_API < MXM_VERSION(2,1)
    if ((MXM_OK != mxm_config_read_context_opts(&mca_spml_ikrit.mxm_ctx_opts)) ||
        (MXM_OK != mxm_config_read_ep_opts(&mca_spml_ikrit.mxm_ep_opts)))
#else
    setenv("MXM_OSHMEM_TLS", mca_spml_ikrit.mxm_tls, 0);
    if (MXM_OK != mxm_config_read_opts(&mca_spml_ikrit.mxm_ctx_opts,
                                       &mca_spml_ikrit.mxm_ep_opts,
                                       "OSHMEM", NULL, 0))
#endif
    {
        SPML_ERROR("Failed to parse MXM configuration");
        return OSHMEM_ERROR;
    }

#if MXM_API < MXM_VERSION(2,0)
    mca_spml_ikrit.mxm_ctx_opts->ptl_bitmap = (MXM_BIT(MXM_PTL_SELF) | MXM_BIT(MXM_PTL_RDMA));
#endif

    err = mxm_init(mca_spml_ikrit.mxm_ctx_opts, &mca_spml_ikrit.mxm_context);
    if (MXM_OK != err) {
        if (MXM_ERR_NO_DEVICE == err) {
            SPML_VERBOSE(1,
                         "No supported device found, disqualifying spml/ikrit");
        } else {
            orte_show_help("help-shmem-spml-ikrit.txt",
                           "mxm init",
                           true,
                           mxm_error_string(err));
        }
        return OSHMEM_ERR_NOT_AVAILABLE;
    }

    err = mxm_mq_create(mca_spml_ikrit.mxm_context,
                        MXM_SHMEM_MQ_ID,
                        &mca_spml_ikrit.mxm_mq);
    if (MXM_OK != err) {
        orte_show_help("help-shmem-spml-ikrit.txt",
                       "mxm mq create",
                       true,
                       mxm_error_string(err));
        return OSHMEM_ERROR;
    }

    return OSHMEM_SUCCESS;
}

static int mca_spml_ikrit_component_close(void)
{
    if (mca_spml_ikrit.mxm_context) {
        mxm_cleanup(mca_spml_ikrit.mxm_context);
#if MXM_API < MXM_VERSION(2,0)
        mxm_config_free(mca_spml_ikrit.mxm_ep_opts);
        mxm_config_free(mca_spml_ikrit.mxm_ctx_opts);
#else
        mxm_config_free_ep_opts(mca_spml_ikrit.mxm_ep_opts);
        mxm_config_free_context_opts(mca_spml_ikrit.mxm_ctx_opts);
#endif
    }
    mca_spml_ikrit.mxm_context = NULL;
    return OSHMEM_SUCCESS;
}

static int spml_ikrit_mxm_init(void)
{
    mxm_error_t err;

#if MXM_API < MXM_VERSION(2,0)
    /* Only relevant for SHM PTL - ignore */
    mca_spml_ikrit.mxm_ep_opts->job_id = 0;
    mca_spml_ikrit.mxm_ep_opts->local_rank = 0;
    mca_spml_ikrit.mxm_ep_opts->num_local_procs = 0;
    mca_spml_ikrit.mxm_ep_opts->rdma.drain_cq = 1;
#endif

    /* Open MXM endpoint */
    err = mxm_ep_create(mca_spml_ikrit.mxm_context,
                        mca_spml_ikrit.mxm_ep_opts,
                        &mca_spml_ikrit.mxm_ep);
    if (MXM_OK != err) {
        orte_show_help("help-shmem-spml-ikrit.txt",
                       "unable to create endpoint",
                       true,
                       mxm_error_string(err));
        return OSHMEM_ERROR;
    }

    return OSHMEM_SUCCESS;
}

static mca_spml_base_module_t*
mca_spml_ikrit_component_init(int* priority,
                              bool enable_progress_threads,
                              bool enable_mpi_threads)
{
    SPML_VERBOSE( 10, "in ikrit, my priority is %d\n", mca_spml_ikrit.priority);

    if ((*priority) > mca_spml_ikrit.priority) {
        *priority = mca_spml_ikrit.priority;
        return NULL ;
    }
    *priority = mca_spml_ikrit.priority;

    if (OSHMEM_SUCCESS != spml_ikrit_mxm_init())
        return NULL ;

    mca_spml_ikrit.n_active_puts = 0;
    mca_spml_ikrit.n_active_gets = 0;
    mca_spml_ikrit.n_mxm_fences = 0;
    SPML_VERBOSE(50, "*** ikrit initialized ****");
    return &mca_spml_ikrit.super;
}

static int mca_spml_ikrit_component_fini(void)
{
    opal_progress_unregister(spml_ikrit_progress);
    if (NULL != mca_spml_ikrit.mxm_ep) {
        mxm_ep_destroy(mca_spml_ikrit.mxm_ep);
    }

    if(!mca_spml_ikrit.enabled)
        return OSHMEM_SUCCESS; /* never selected.. return success.. */
    mca_spml_ikrit.enabled = false;  /* not anymore */

    return OSHMEM_SUCCESS;
}

