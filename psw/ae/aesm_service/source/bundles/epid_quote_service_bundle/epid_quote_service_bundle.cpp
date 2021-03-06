#include <epid_quote_service.h>
#include <network_service.h>
#include <pce_service.h>

#include <cppmicroservices/BundleActivator.h>
#include "cppmicroservices/BundleContext.h"
#include <cppmicroservices/GetBundleContext.h>

#include <iostream>
#include <inttypes.h>
#include "aesm_xegd_blob.h"
#include "aesm_logic.h"
#include "pve_logic.h"
#include "qe_logic.h"
#include "aesm_epid_blob.h"
#include "aesm_long_lived_thread.h"
#include "QEClass.h"
#include "PVEClass.h"
#include "util.h"
#include "launch_service.h"
#include "pce_service.h"
#include "es_info.h"
#include "endpoint_select_info.h"
#include "cppmicroservices_util.h"

using namespace cppmicroservices;
std::shared_ptr<INetworkService> g_network_service;
std::shared_ptr<IPceService> g_pce_service;
std::shared_ptr<ILaunchService> g_launch_service;


extern ThreadStatus epid_thread;
static uint32_t active_extended_epid_group_id = 0;
static AESMLogicMutex _qe_pve_mutex;

#define CHECK_EPID_PROVISIONG_STATUS \
    if(!query_pve_thread_status()){\
        return AESM_BUSY;\
    }

bool query_pve_thread_status(void)
{
    return epid_thread.query_status_and_reset_clock();
}


uint32_t get_active_extended_epid_group_id_internal()
{
    return active_extended_epid_group_id;
}

static ae_error_t thread_to_load_qe(aesm_thread_arg_type_t arg)
{
    epid_blob_with_cur_psvn_t epid_data;
    ae_error_t ae_ret = AE_FAILURE;
    UNUSED(arg);
    AESM_DBG_TRACE("start to load qe");
    memset(&epid_data, 0, sizeof(epid_data));
    AESMLogicLock lock(_qe_pve_mutex);
    if((ae_ret = EPIDBlob::instance().read(epid_data)) == AE_SUCCESS)
    {
        AESM_DBG_TRACE("EPID blob is read successfully, loading QE ...");
        ae_ret = CQEClass::instance().load_enclave();
        if(AE_SUCCESS != ae_ret)
        {
            AESM_DBG_WARN("fail to load QE: %d", ae_ret);
        }else{
            AESM_DBG_TRACE("QE loaded successfully");
            bool resealed = false;
            sgx_cpu_svn_t cpusvn;
            memset(&cpusvn, 0, sizeof(cpusvn));
            se_static_assert(SGX_TRUSTED_EPID_BLOB_SIZE_SDK>=SGX_TRUSTED_EPID_BLOB_SIZE_SIK);
            // Just take this chance to reseal EPID blob in case TCB is
            // upgraded, return value is ignored and no provisioning is
            // triggered.
            ae_ret = static_cast<ae_error_t>(CQEClass::instance().verify_blob(
                epid_data.trusted_epid_blob,
                SGX_TRUSTED_EPID_BLOB_SIZE_SDK,
                &resealed, &cpusvn));
            if(AE_SUCCESS != ae_ret)
            {
                AESM_DBG_WARN("Failed to verify EPID blob: %d", ae_ret);
                EPIDBlob::instance().remove();
            }else{
                uint32_t epid_xeid;
                // Check whether EPID blob XEGDID is aligned with active extended group id if it exists.
                if ((EPIDBlob::instance().get_extended_epid_group_id(&epid_xeid) == AE_SUCCESS) && (epid_xeid == get_active_extended_epid_group_id_internal())) {
                    AESM_DBG_TRACE("EPID blob Verified");
                    // XEGDID is aligned
                    if (true == resealed)
                    {
                        AESM_DBG_TRACE("EPID blob is resealed");
                        if ((ae_ret = EPIDBlob::instance().write(epid_data))
                            != AE_SUCCESS)
                        {
                            AESM_DBG_WARN("Failed to update epid blob: %d", ae_ret);
                        }
                    }
                }
                else { // XEGDID is NOT aligned
                    AESM_DBG_TRACE("XEGDID mismatch in EPIDBlob, remove it...");
                    EPIDBlob::instance().remove();
                }
            }
        }
    }else{
        AESM_DBG_TRACE("Fail to read EPID Blob");
    }
    AESM_DBG_TRACE("QE Thread finished succ");
    return AE_SUCCESS;
}

static ae_error_t read_global_extended_epid_group_id(uint32_t *xeg_id)
{
    char path_name[MAX_PATH];
    ae_error_t ae_ret = aesm_get_pathname(FT_PERSISTENT_STORAGE, EXTENDED_EPID_GROUP_ID_FID, path_name, MAX_PATH);
    if(AE_SUCCESS != ae_ret){
        return ae_ret;
    }
    FILE * f = fopen(path_name, "r");
    if( f == NULL){
        return OAL_CONFIG_FILE_ERROR;
    }
    ae_ret = OAL_CONFIG_FILE_ERROR;
    if(fscanf(f, "%" SCNu32, xeg_id)==1){
        ae_ret = AE_SUCCESS;
    }
    fclose(f);
    return ae_ret;
}
static ae_error_t set_global_extended_epid_group_id(uint32_t xeg_id)
{
    char path_name[MAX_PATH];
    ae_error_t ae_ret = aesm_get_pathname(FT_PERSISTENT_STORAGE, EXTENDED_EPID_GROUP_ID_FID, path_name, MAX_PATH);
    if(AE_SUCCESS != ae_ret){
        return ae_ret;
    }
    FILE *f = fopen(path_name, "w");
    if(f == NULL){
        return OAL_CONFIG_FILE_ERROR;
    }
    ae_ret = OAL_CONFIG_FILE_ERROR;
    if(fprintf(f, "%" PRIu32, xeg_id)>0){
        ae_ret = AE_SUCCESS;
    }
    fclose(f);
    return ae_ret;
}


class EpidQuoteServiceImp : public IEpidQuoteService
{
private:
    bool initialized;
    aesm_thread_t qe_thread;
public:
    EpidQuoteServiceImp():initialized(false), qe_thread(NULL){}


    ae_error_t start()
    {
        ae_error_t ae_ret = AE_SUCCESS;
        if (initialized == true)
        {
            AESM_DBG_INFO("epid bundle has been started");
            return AE_SUCCESS;
        }

        AESM_DBG_INFO("Starting epid bundle");
        get_service_wrapper(g_network_service);
        get_service_wrapper(g_launch_service);
        get_service_wrapper(g_pce_service);

        if (g_launch_service)
            g_launch_service->start();

        ae_ret = read_global_extended_epid_group_id(&active_extended_epid_group_id);
        if (AE_SUCCESS != ae_ret){
            AESM_DBG_INFO("Fail to read extended epid group id, default extended epid group used");
            active_extended_epid_group_id = DEFAULT_EGID; // Use default extended epid group id 0 if it is not available from data file
        }
        else{
            AESM_DBG_INFO("active extended group id %d used", active_extended_epid_group_id);
        }
        if (AE_SUCCESS != (XEGDBlob::verify_xegd_by_xgid(active_extended_epid_group_id)) ||
            AE_SUCCESS != (EndpointSelectionInfo::verify_file_by_xgid(active_extended_epid_group_id))){// Try to load XEGD and URL file to make sure it is valid
            AESM_LOG_WARN_ADMIN("%s", g_admin_event_string_table[SGX_ADMIN_EVENT_PCD_NOT_AVAILABLE]);
            AESM_LOG_WARN("%s: original extended epid group id = %d", g_event_string_table[SGX_EVENT_PCD_NOT_AVAILABLE], active_extended_epid_group_id);
            active_extended_epid_group_id = DEFAULT_EGID;// If the active extended epid group id read from data file is not valid, switch to default extended epid group id
        }

        ae_error_t aesm_ret = aesm_create_thread(thread_to_load_qe, 0,&qe_thread);
        if(AE_SUCCESS != aesm_ret){
            AESM_DBG_WARN("Fail to create thread to preload QE:(ae %d)",aesm_ret);
            return AE_FAILURE;
        }
        initialized = true;
        AESM_DBG_INFO("epid bundle started");
        return AE_SUCCESS;
    }
    void stop()
    {
        ae_error_t ae_ret, thread_ret;

        ae_ret = aesm_wait_thread(qe_thread, &thread_ret, AESM_STOP_TIMEOUT);
        if (ae_ret != AE_SUCCESS || thread_ret != AE_SUCCESS)
        {
            AESM_DBG_INFO("aesm_wait_thread failed(qe_thread):(ae %d) (%d)", ae_ret, thread_ret);
        }
        (void)aesm_free_thread(qe_thread);//release thread handle to free memory

        uint64_t stop_tick_count = se_get_tick_count()+500/1000;
        epid_thread.stop_thread(stop_tick_count);

        CPVEClass::instance().unload_enclave();
        CQEClass::instance().unload_enclave();
        AESM_DBG_INFO("epid bundle stopped");
    }

    aesm_error_t init_quote(
        uint8_t *target_info,
        uint32_t target_info_size,
        uint8_t *gid,
        uint32_t gid_size)
    {
        ae_error_t ret = AE_SUCCESS;
        sgx_target_info_t pce_target_info;
        uint16_t pce_isv_svn = 0xFFFF;
        memset(&pce_target_info, 0, sizeof(pce_target_info));
        AESM_DBG_INFO("init_quote");
        if(sizeof(sgx_target_info_t) != target_info_size ||
           sizeof(sgx_epid_group_id_t) != gid_size)
        {
            return AESM_PARAMETER_ERROR;
        }
        AESMLogicLock lock(_qe_pve_mutex);
        CHECK_EPID_PROVISIONG_STATUS;
        if(!g_pce_service)
            return AESM_SERVICE_UNAVAILABLE;
        ret = g_pce_service->load_enclave();
        if(AE_SUCCESS == ret)
            ret = static_cast<ae_error_t>(g_pce_service->pce_get_target(&pce_target_info, &pce_isv_svn));
        if(AE_SUCCESS != ret)
        {
            if(AESM_AE_OUT_OF_EPC == ret)
                return AESM_OUT_OF_EPC;
            else if(AESM_AE_NO_DEVICE == ret)
                return AESM_NO_DEVICE_ERROR;
            else if(AE_SERVER_NOT_AVAILABLE == ret)
                return AESM_SERVICE_UNAVAILABLE;
            return AESM_UNEXPECTED_ERROR;
        }
        return QEAESMLogic::init_quote(
                   reinterpret_cast<sgx_target_info_t *>(target_info),
                   gid, gid_size, pce_isv_svn);
    }

    aesm_error_t get_quote(
        const uint8_t *report, uint32_t report_size,
        uint32_t quote_type,
        const uint8_t *spid, uint32_t spid_size,
        const uint8_t *nonce, uint32_t nonce_size,
        const uint8_t *sigrl, uint32_t sigrl_size,
        uint8_t *qe_report, uint32_t qe_report_size,
        uint8_t *quote, uint32_t buf_size)
    {
        ae_error_t ret = AE_SUCCESS;
        sgx_target_info_t pce_target_info;
        uint16_t pce_isv_svn = 0xFFFF;
        memset(&pce_target_info, 0, sizeof(pce_target_info));
        AESM_DBG_INFO("get_quote");
        if(sizeof(sgx_report_t) != report_size ||
           sizeof(sgx_spid_t) != spid_size)
        {
            return AESM_PARAMETER_ERROR;
        }
        if((nonce && sizeof(sgx_quote_nonce_t) != nonce_size)
            || (qe_report && sizeof(sgx_report_t) != qe_report_size))

        {
            return AESM_PARAMETER_ERROR;
        }
        AESMLogicLock lock(_qe_pve_mutex);
        CHECK_EPID_PROVISIONG_STATUS;
        if(!g_pce_service)
            return AESM_SERVICE_UNAVAILABLE;

        ret = g_pce_service->load_enclave();
        if(AE_SUCCESS == ret)
            ret = static_cast<ae_error_t>(g_pce_service->pce_get_target(&pce_target_info, &pce_isv_svn));
        if(AE_SUCCESS != ret)
        {
            if(AESM_AE_OUT_OF_EPC == ret)
                return AESM_OUT_OF_EPC;
            else if(AESM_AE_NO_DEVICE == ret)
                return AESM_NO_DEVICE_ERROR;
            else if(AE_SERVER_NOT_AVAILABLE == ret)
                return AESM_SERVICE_UNAVAILABLE;
            return AESM_UNEXPECTED_ERROR;
        }
        return QEAESMLogic::get_quote(report, quote_type, spid, nonce, sigrl,
                                      sigrl_size, qe_report, quote, buf_size, pce_isv_svn);
    }

    aesm_error_t get_extended_epid_group_id(
        uint32_t* extended_epid_group_id)
    {
        AESM_DBG_INFO("get_extended_epid_group");
        if (NULL == extended_epid_group_id)
        {
            return AESM_PARAMETER_ERROR;
        }
        *extended_epid_group_id = get_active_extended_epid_group_id_internal();
        return AESM_SUCCESS;
    }

    aesm_error_t switch_extended_epid_group(
        uint32_t extended_epid_group_id)
    {
        AESM_DBG_INFO("switch_extended_epid_group");
        ae_error_t ae_ret;
        if ((ae_ret = XEGDBlob::verify_xegd_by_xgid(extended_epid_group_id)) != AE_SUCCESS ||
            (ae_ret = EndpointSelectionInfo::verify_file_by_xgid(extended_epid_group_id)) != AE_SUCCESS){
            AESM_DBG_INFO("Fail to switch to extended epid group to %d due to XEGD blob for URL blob not available", extended_epid_group_id);
            return AESM_PARAMETER_ERROR;
        }
        ae_ret = set_global_extended_epid_group_id(extended_epid_group_id);
        if (ae_ret != AE_SUCCESS){
            AESM_DBG_INFO("Fail to switch to extended epid group %d", extended_epid_group_id);
            return AESM_UNEXPECTED_ERROR;
        }

        AESM_DBG_INFO("Succ to switch to extended epid group %d in data file, restart aesm required to use it", extended_epid_group_id);
        return AESM_SUCCESS;
    }

    uint32_t endpoint_selection(
        endpoint_selection_infos_t& es_info)
    {
        AESMLogicLock lock(_qe_pve_mutex);
        SGX_DBGPRINT_ONE_STRING_TWO_INTS_ENDPOINT_SELECTION(__FUNCTION__" (line, 0)", __LINE__, 0);
        return EndpointSelectionInfo::instance().start_protocol(es_info);
    }
    aesm_error_t provision(
        bool performance_rekey_used,
        uint32_t timeout_usec)
    {
        AESMLogicLock lock(_qe_pve_mutex);
        CHECK_EPID_PROVISIONG_STATUS;
        return PvEAESMLogic::provision(performance_rekey_used, timeout_usec);
    }
    const char *get_server_url(
        aesm_network_server_enum_type_t type)
    {
        return EndpointSelectionInfo::instance().get_server_url(type);
    }
    const char *get_pse_provisioning_url(
        const endpoint_selection_infos_t& es_info)
    {
        return EndpointSelectionInfo::instance().get_pse_provisioning_url(es_info);
    }
    uint32_t is_gid_matching_result_in_epid_blob(
        const GroupId& gid)
    {
        AESMLogicLock lock(_qe_pve_mutex);
        EPIDBlob& epid_blob = EPIDBlob::instance();
        uint32_t le_gid;
        if(epid_blob.get_sgx_gid(&le_gid)!=AE_SUCCESS){//get littlen endian gid
            return GIDMT_UNEXPECTED_ERROR;
        }
        le_gid=_htonl(le_gid);//use bigendian gid
        se_static_assert(sizeof(le_gid)==sizeof(gid));
        if(memcmp(&le_gid,&gid,sizeof(gid))!=0){
            return GIDMT_UNMATCHED;
        }
        return GIDMT_MATCHED;
    }
};

class Activator : public BundleActivator
{
  void Start(BundleContext ctx)
  {
    auto service = std::make_shared<EpidQuoteServiceImp>();
    ctx.RegisterService<IEpidQuoteService>(service);
  }

  void Stop(BundleContext)
  {
    // Nothing to do
  }
};

CPPMICROSERVICES_EXPORT_BUNDLE_ACTIVATOR(Activator)

// [no-cmake]
// The code below is required if the CMake
// helper functions are not used.
#ifdef NO_CMAKE
CPPMICROSERVICES_INITIALIZE_BUNDLE(epid_quote_service_bundle_name)
#endif
