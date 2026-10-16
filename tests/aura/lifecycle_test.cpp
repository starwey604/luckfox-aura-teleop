// Exercise the production sender against a stateful MPI fake. This is not a
// codec/hardware substitute: it checks ownership, rollback, and join ordering.
#define main aura_sender_main
#include "../../src/aura-sender/src/mpp_sender.cpp"
#undef main
#include <algorithm>
#include <mutex>

namespace mock {
struct Camera {
    bool device{}, vi{}, venc{}, receiving{}, bound{}, leased{};
    unsigned gets{};
    unsigned split_mode{};
    VENC_CHN_ATTR_S attributes{};
    VENC_RC_PARAM_S rc{};
    VENC_H264_ENTROPY_S entropy{1U, 0U};
    VENC_H264_TRANS_S transform{};
    VENC_H264_DBLK_S deblock{};
    VENC_SLICE_SPLIT_S split{};
};
std::array<Camera, 2> cameras;
std::mutex mutex;
bool system{};
bool incomplete_frame_pack{};
bool ignore_codec_writes{};
unsigned init_count{}, exit_count{}, wait_count{}, release_count{};
std::string fail_api;
std::vector<std::string> events;

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
RK_S32 call(std::string_view name)
{
    events.emplace_back(name);
    if (fail_api == name) { fail_api.clear(); return -777; }
    return RK_SUCCESS;
}
void reset()
{
    cameras = {};
    init_count = exit_count = wait_count = release_count = 0;
    system = false;
    incomplete_frame_pack = false;
    ignore_codec_writes = false;
    events.clear();
    fail_api.clear();
    g_stop_requested.store(false);
    g_cleanup_failed.store(false);
    unsetenv("AURA_FAIL_AFTER");
}
void all_released()
{
    require(!system, "system leaked");
    for (const auto& c : cameras) {
        require(!c.device && !c.vi && !c.venc && !c.receiving && !c.bound && !c.leased,
                "camera resource leaked");
    }
    require(init_count == 1 && exit_count == 1 && wait_count == 1, "SYS lifecycle count mismatch");
}
}
extern "C" {
RK_S32 RK_MPI_SYS_Init() { std::lock_guard lock(mock::mutex); mock::require(!mock::system, "double SYS_Init"); mock::system=true; ++mock::init_count; return mock::call("SYS_Init"); }
RK_S32 RK_MPI_SYS_WaitFreeMB() {
    std::lock_guard lock(mock::mutex);
    for (const auto& c : mock::cameras) mock::require(!c.vi && !c.venc && !c.leased, "WaitFreeMB before cleanup");
    ++mock::wait_count; return mock::call("SYS_WaitFreeMB");
}
RK_S32 RK_MPI_SYS_Exit() { std::lock_guard lock(mock::mutex); mock::system=false; ++mock::exit_count; return mock::call("SYS_Exit"); }
RK_S32 RK_MPI_SYS_Bind(const MPP_CHN_S* src, const MPP_CHN_S*) { std::lock_guard lock(mock::mutex); const auto r=mock::call("SYS_Bind"); if (r==0) mock::cameras.at(static_cast<unsigned>(src->s32DevId)).bound=true; return r; }
RK_S32 RK_MPI_SYS_UnBind(const MPP_CHN_S* src, const MPP_CHN_S*) { std::lock_guard lock(mock::mutex); mock::cameras.at(static_cast<unsigned>(src->s32DevId)).bound=false; return mock::call("SYS_UnBind"); }
RK_S32 RK_MPI_VI_GetDevAttr(VI_DEV, VI_DEV_ATTR_S*) { return RK_ERR_VI_NOT_CONFIG; }
RK_S32 RK_MPI_VI_SetDevAttr(VI_DEV, const VI_DEV_ATTR_S*) { std::lock_guard lock(mock::mutex); return mock::call("VI_SetDevAttr"); }
RK_S32 RK_MPI_VI_GetDevIsEnable(VI_DEV) { return -1; }
RK_S32 RK_MPI_VI_EnableDev(VI_DEV id) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VI_EnableDev"); if (r==0) mock::cameras.at(static_cast<unsigned>(id)).device=true; return r; }
RK_S32 RK_MPI_VI_DisableDev(VI_DEV id) { std::lock_guard lock(mock::mutex); mock::cameras.at(static_cast<unsigned>(id)).device=false; return mock::call("VI_DisableDev"); }
RK_S32 RK_MPI_VI_SetDevBindPipe(VI_DEV, const VI_DEV_BIND_PIPE_S*) { std::lock_guard lock(mock::mutex); return mock::call("VI_SetDevBindPipe"); }
RK_S32 RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN, const VI_CHN_ATTR_S*) { std::lock_guard lock(mock::mutex); return mock::call("VI_SetChnAttr"); }
RK_S32 RK_MPI_VI_SetChnWrapBufAttr(VI_PIPE, VI_CHN, const VI_CHN_BUF_WRAP_S*) { std::lock_guard lock(mock::mutex); return mock::call("VI_SetChnWrapBufAttr"); }
RK_S32 RK_MPI_VI_EnableChn(VI_PIPE id, VI_CHN) { std::lock_guard lock(mock::mutex); for (const auto& c : mock::cameras) mock::require(!c.bound, "VI enabled after encoder binding"); const auto r=mock::call("VI_EnableChn"); if (r==0) mock::cameras.at(static_cast<unsigned>(id)).vi=true; return r; }
RK_S32 RK_MPI_VI_DisableChn(VI_PIPE id, VI_CHN) { std::lock_guard lock(mock::mutex); mock::cameras.at(static_cast<unsigned>(id)).vi=false; return mock::call("VI_DisableChn"); }
RK_S32 RK_MPI_VENC_CreateChn(VENC_CHN id, const VENC_CHN_ATTR_S* attr) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_CreateChn"); if (r==0) { auto& c=mock::cameras.at(static_cast<unsigned>(id)); c.venc=true; c.attributes=*attr; } return r; }
RK_S32 RK_MPI_VENC_DestroyChn(VENC_CHN id) { std::lock_guard lock(mock::mutex); auto& c=mock::cameras.at(static_cast<unsigned>(id)); mock::require(!c.leased && !c.receiving && !c.bound, "destroy before release/stop"); c.venc=false; return mock::call("VENC_DestroyChn"); }
RK_S32 RK_MPI_VENC_StartRecvFrame(VENC_CHN id, const VENC_RECV_PIC_PARAM_S*) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_StartRecvFrame"); if (r==0) mock::cameras.at(static_cast<unsigned>(id)).receiving=true; return r; }
RK_S32 RK_MPI_VENC_StopRecvFrame(VENC_CHN id) { std::lock_guard lock(mock::mutex); auto& c=mock::cameras.at(static_cast<unsigned>(id)); mock::require(!c.leased, "stop before stream released"); c.receiving=false; return mock::call("VENC_StopRecvFrame"); }
RK_S32 RK_MPI_VENC_SetChnBufWrapAttr(VENC_CHN, const VENC_CHN_BUF_WRAP_S*) { std::lock_guard lock(mock::mutex); return mock::call("VENC_SetChnBufWrapAttr"); }
RK_S32 RK_MPI_VENC_SetChnRefBufShareAttr(VENC_CHN, const VENC_CHN_REF_BUF_SHARE_S*) { std::lock_guard lock(mock::mutex); return mock::call("VENC_SetChnRefBufShareAttr"); }
RK_S32 RK_MPI_VENC_GetChnAttr(VENC_CHN id, VENC_CHN_ATTR_S* value) { std::lock_guard lock(mock::mutex); *value=mock::cameras.at(static_cast<unsigned>(id)).attributes; return mock::call("VENC_GetChnAttr"); }
RK_S32 RK_MPI_VENC_GetSliceSplit(VENC_CHN id, VENC_SLICE_SPLIT_S* value) { std::lock_guard lock(mock::mutex); *value=mock::cameras.at(static_cast<unsigned>(id)).split; return mock::call("VENC_GetSliceSplit"); }
RK_S32 RK_MPI_VENC_SetSliceSplit(VENC_CHN id, const VENC_SLICE_SPLIT_S* split) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_SetSliceSplit"); if (r==0) { auto& c=mock::cameras.at(static_cast<unsigned>(id)); c.split_mode=split->u32SplitMode; c.split=*split; } return r; }
RK_S32 RK_MPI_VENC_GetRcParam(VENC_CHN id, VENC_RC_PARAM_S* value) { std::lock_guard lock(mock::mutex); *value=mock::cameras.at(static_cast<unsigned>(id)).rc; return mock::call("VENC_GetRcParam"); }
RK_S32 RK_MPI_VENC_SetRcParam(VENC_CHN id, const VENC_RC_PARAM_S* value) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_SetRcParam"); if (r==0 && !mock::ignore_codec_writes) mock::cameras.at(static_cast<unsigned>(id)).rc=*value; return r; }
RK_S32 RK_MPI_VENC_GetH264Entropy(VENC_CHN id, VENC_H264_ENTROPY_S* value) { std::lock_guard lock(mock::mutex); *value=mock::cameras.at(static_cast<unsigned>(id)).entropy; return mock::call("VENC_GetH264Entropy"); }
RK_S32 RK_MPI_VENC_SetH264Entropy(VENC_CHN id, const VENC_H264_ENTROPY_S* value) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_SetH264Entropy"); if (r==0 && !mock::ignore_codec_writes) mock::cameras.at(static_cast<unsigned>(id)).entropy=*value; return r; }
RK_S32 RK_MPI_VENC_GetH264Trans(VENC_CHN id, VENC_H264_TRANS_S* value) { std::lock_guard lock(mock::mutex); *value=mock::cameras.at(static_cast<unsigned>(id)).transform; return mock::call("VENC_GetH264Trans"); }
RK_S32 RK_MPI_VENC_SetH264Trans(VENC_CHN id, const VENC_H264_TRANS_S* value) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_SetH264Trans"); if (r==0 && !mock::ignore_codec_writes) mock::cameras.at(static_cast<unsigned>(id)).transform=*value; return r; }
RK_S32 RK_MPI_VENC_GetH264Dblk(VENC_CHN id, VENC_H264_DBLK_S* value) { std::lock_guard lock(mock::mutex); *value=mock::cameras.at(static_cast<unsigned>(id)).deblock; return mock::call("VENC_GetH264Dblk"); }
RK_S32 RK_MPI_VENC_SetH264Dblk(VENC_CHN id, const VENC_H264_DBLK_S* value) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_SetH264Dblk"); if (r==0 && !mock::ignore_codec_writes) mock::cameras.at(static_cast<unsigned>(id)).deblock=*value; return r; }
RK_S32 RK_MPI_VENC_QueryStatus(VENC_CHN, VENC_CHN_STATUS_S*) { return 0; }
RK_S32 RK_MPI_VENC_GetStream(VENC_CHN id, VENC_STREAM_S* stream, RK_S32) {
    if (id==1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::lock_guard lock(mock::mutex);
    const auto result=mock::call("VENC_GetStream"); if (result!=0) return result;
    auto& c=mock::cameras.at(static_cast<unsigned>(id));
    mock::require(c.bound && c.receiving && !c.leased, "invalid stream acquisition");
    c.leased=true; ++c.gets;
    stream->u32PackCount=1;
    stream->pstPack[0].pMbBlk=reinterpret_cast<MB_BLK>(1);
    stream->pstPack[0].u32Len=4;
    stream->pstPack[0].bFrameEnd=mock::incomplete_frame_pack ? RK_FALSE : RK_TRUE;
    stream->pstPack[0].u64PTS=(c.split_mode==3 ? (c.gets-1)/2+1 : c.gets)*10'000ULL;
    return 0;
}
RK_S32 RK_MPI_VENC_ReleaseStream(VENC_CHN id, VENC_STREAM_S*) { std::lock_guard lock(mock::mutex); auto& c=mock::cameras.at(static_cast<unsigned>(id)); mock::require(c.leased, "double stream release"); c.leased=false; ++mock::release_count; return mock::call("VENC_ReleaseStream"); }
RK_VOID* RK_MPI_MB_Handle2VirAddr(MB_BLK) { static unsigned char payload[4]{0,0,0,1}; return payload; }
}

int run(bool dual=false, const char* output_mode="frame", const std::vector<std::string>& extra={})
{
    std::vector<std::string> arguments{"aura-sender-test", "--width", "1280", "--height", "720", "--wrap-lines", "0", "--frames", "3"};
    if (dual) arguments.emplace_back("--dual");
    arguments.emplace_back("--output-mode"); arguments.emplace_back(output_mode);
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    std::vector<char*> pointers;
    for (auto& arg : arguments) pointers.push_back(arg.data());
    return aura_sender_main(static_cast<int>(pointers.size()), pointers.data());
}
int main()
{
    try {
        teleop::manual_isp::Settings isp;
        mock::require(!isp.enabled(), "ISP changed by default");
        for (const auto* bad : {"1,1,1", "1,1,1,1,1", "nan,1,1,1", "9,1,1,1", "0,1,1,1"})
            mock::require(!teleop::manual_isp::set_wb(isp, bad), "bad WB accepted");
        mock::require(teleop::manual_isp::set_wb(isp, "1.4375,1,1,1.5625"), "valid WB rejected");
        for (const auto* bad : {"-1,0,0,0", "4096,0,0,0", "1.5,0,0,0"})
            mock::require(!teleop::manual_isp::set_black(isp, bad), "bad BLS accepted");
        mock::require(teleop::manual_isp::set_black(isp, "172,168,168,172"), "valid BLS rejected");
        const auto params = teleop::manual_isp::make_params(isp);
        mock::require(params.module_ens == (ISP35_MODULE_AWB_GAIN | ISP35_MODULE_BLS) &&
                      params.module_cfg_update == params.module_ens && params.module_en_update == params.module_ens,
                      "ISP overwrites unrelated module masks");
        mock::require(params.others.awb_gain_cfg.gain0_red == 368 && params.others.awb_gain_cfg.gain2_blue == 400 &&
                      params.others.awb_gain_cfg.awb1_gain_r == 256 && params.others.awb_gain_cfg.awb1_gain_b == 256,
                      "WB quantization or secondary unity stage wrong");
        mock::require(params.others.bls_cfg.fixed_val.r == 172 && params.others.bls_cfg.fixed_val.gr == 168 &&
                      !params.others.bls_cfg.bls1_en && params.others.bls_cfg.isp_ob_predgain == 0,
                      "BLS affects extra stages");
        mock::reset(); setenv("AURA_FAIL_AFTER", "system.initialized", 1);
        mock::require(run()!=0, "system initialization injection not detected"); mock::all_released();
        mock::require(mock::events == std::vector<std::string>{"SYS_Init", "SYS_WaitFreeMB", "SYS_Exit"},
                      "system-only probe opened a camera or repeated cleanup");
        for (const auto* phase : {"isp.prepared", "vi.device", "vi.enabled", "venc.created", "venc.started", "bound", "stream.acquired"}) {
            mock::reset(); setenv("AURA_FAIL_AFTER", phase, 1);
            mock::require(run()!=0, "injection not detected"); mock::all_released();
        }
        for (const auto* api : {"VI_SetDevAttr", "VI_EnableDev", "VI_SetDevBindPipe", "VI_SetChnAttr", "VI_EnableChn", "VENC_CreateChn", "VENC_SetChnRefBufShareAttr", "VENC_GetSliceSplit", "VENC_SetSliceSplit", "VENC_StartRecvFrame", "SYS_Bind", "VENC_GetStream", "VENC_ReleaseStream"}) {
            mock::reset(); mock::fail_api=api;
            mock::require(run()!=0, "API error not detected"); mock::all_released();
        }
        mock::reset(); setenv("AURA_FAIL_AFTER", "camera1.venc.created", 1);
        mock::require(run(true)!=0, "second-camera injection not detected"); mock::all_released();
        mock::reset(); mock::fail_api="VENC_DestroyChn";
        mock::require(run()!=0 && g_cleanup_failed.load(), "cleanup failure not reported"); mock::all_released();
        mock::reset(); mock::require(run(true)==0, "dual worker failed"); mock::all_released();
        mock::require(mock::cameras[0].gets>3 && mock::cameras[1].gets==3, "completed stream did not continue draining");
        mock::reset(); mock::require(run()==0, "frame mode failed"); mock::all_released();
        mock::require(mock::cameras[0].split_mode==1 && mock::cameras[0].gets==3,
                      "frame mode waited for a fourth frame before sending three");
        mock::reset(); mock::require(run(false, "slice")==0, "slice compatibility failed"); mock::all_released();
        mock::require(mock::cameras[0].split_mode==3 && mock::cameras[0].gets==7,
                      "slice mode trusted per-slice bFrameEnd instead of grouping PTS");
        const std::vector<std::string> codec_args{"--rc-mode", "fixqp", "--qp-i", "22", "--qp-p", "26", "--gop", "1", "--profile", "baseline", "--deblock", "off", "--max-reencode", "0", "--slice-ctus", "0", "--codec-report"};
        mock::reset(); mock::require(run(true, "frame", codec_args)==0, "codec configuration failed"); mock::all_released();
        for (const auto& c : mock::cameras) {
            mock::require(c.attributes.stRcAttr.enRcMode==VENC_RC_MODE_H264FIXQP &&
                          c.attributes.stRcAttr.stH264FixQp.u32Gop==1 &&
                          c.attributes.stRcAttr.stH264FixQp.u32IQp==22 &&
                          c.attributes.stRcAttr.stH264FixQp.u32PQp==26 &&
                          c.attributes.stVencAttr.u32Profile==66, "wrong codec union/profile");
            mock::require(c.entropy.u32EntropyEncMode==0 && c.transform.u32TransMode==1 &&
                          c.deblock.disable_deblocking_filter_idc==1 && !c.split.bSplitEnable, "codec setting lost");
        }
        for (const auto* api : {"VENC_GetRcParam", "VENC_SetRcParam", "VENC_GetH264Entropy", "VENC_SetH264Entropy", "VENC_GetH264Trans", "VENC_SetH264Trans", "VENC_GetH264Dblk", "VENC_SetH264Dblk", "VENC_GetChnAttr"}) {
            mock::reset(); mock::fail_api=api;
            mock::require(run(false, "frame", codec_args)!=0, "codec API failure ignored"); mock::all_released();
        }
        mock::reset(); mock::ignore_codec_writes=true;
        mock::require(run(false, "frame", {"--entropy", "cavlc"})!=0, "silent readback mismatch accepted"); mock::all_released();
        mock::reset(); mock::fail_api="VENC_GetH264Dblk";
        mock::require(run(false, "frame", {"--codec-report"})==0, "unsupported optional getter must be reported, not change baseline"); mock::all_released();
        for (const std::vector<std::string>& args : {std::vector<std::string>{"--profile", "baseline", "--entropy", "cabac"},
                {"--profile", "main", "--transform", "auto"}, {"--qp-i", "20"}, {"--max-reencode", "4"},
                {"--rc-mode", "fixqp", "--qp-p", "52"}, {"--isp-wb", "nan,1,1,1"},
                {"--isp-wb", "1,1,1"}, {"--isp-black-level", "1.5,0,0,0"}}) {
            mock::reset(); mock::require(run(false, "frame", args)!=0 && mock::init_count==0, "invalid codec args reached MPI");
        }
        mock::reset(); mock::incomplete_frame_pack=true;
        mock::require(run()!=0, "partial frame pack accepted as complete AU"); mock::all_released();
        mock::reset();
        { MppSession first; bool rejected=false;
          try { MppSession second; } catch (const std::exception&) { rejected=true; }
          mock::require(rejected, "second MPI session accepted");
        }
        mock::all_released();
        std::puts("lifecycle-test: PASS rollback/API failure/stream lease/dual drain/process lock/cleanup status");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "lifecycle-test: %s\n", error.what()); return 1; }
}
