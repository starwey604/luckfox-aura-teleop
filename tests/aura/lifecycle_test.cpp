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
};
std::array<Camera, 2> cameras;
std::mutex mutex;
bool system{};
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
RK_S32 RK_MPI_VENC_CreateChn(VENC_CHN id, const VENC_CHN_ATTR_S*) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_CreateChn"); if (r==0) mock::cameras.at(static_cast<unsigned>(id)).venc=true; return r; }
RK_S32 RK_MPI_VENC_DestroyChn(VENC_CHN id) { std::lock_guard lock(mock::mutex); auto& c=mock::cameras.at(static_cast<unsigned>(id)); mock::require(!c.leased && !c.receiving && !c.bound, "destroy before release/stop"); c.venc=false; return mock::call("VENC_DestroyChn"); }
RK_S32 RK_MPI_VENC_StartRecvFrame(VENC_CHN id, const VENC_RECV_PIC_PARAM_S*) { std::lock_guard lock(mock::mutex); const auto r=mock::call("VENC_StartRecvFrame"); if (r==0) mock::cameras.at(static_cast<unsigned>(id)).receiving=true; return r; }
RK_S32 RK_MPI_VENC_StopRecvFrame(VENC_CHN id) { std::lock_guard lock(mock::mutex); auto& c=mock::cameras.at(static_cast<unsigned>(id)); mock::require(!c.leased, "stop before stream released"); c.receiving=false; return mock::call("VENC_StopRecvFrame"); }
RK_S32 RK_MPI_VENC_SetChnBufWrapAttr(VENC_CHN, const VENC_CHN_BUF_WRAP_S*) { std::lock_guard lock(mock::mutex); return mock::call("VENC_SetChnBufWrapAttr"); }
RK_S32 RK_MPI_VENC_SetChnRefBufShareAttr(VENC_CHN, const VENC_CHN_REF_BUF_SHARE_S*) { std::lock_guard lock(mock::mutex); return mock::call("VENC_SetChnRefBufShareAttr"); }
RK_S32 RK_MPI_VENC_GetSliceSplit(VENC_CHN, VENC_SLICE_SPLIT_S*) { std::lock_guard lock(mock::mutex); return mock::call("VENC_GetSliceSplit"); }
RK_S32 RK_MPI_VENC_SetSliceSplit(VENC_CHN, const VENC_SLICE_SPLIT_S*) { std::lock_guard lock(mock::mutex); return mock::call("VENC_SetSliceSplit"); }
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
    stream->pstPack[0].u64PTS=c.gets*10'000ULL;
    return 0;
}
RK_S32 RK_MPI_VENC_ReleaseStream(VENC_CHN id, VENC_STREAM_S*) { std::lock_guard lock(mock::mutex); auto& c=mock::cameras.at(static_cast<unsigned>(id)); mock::require(c.leased, "double stream release"); c.leased=false; ++mock::release_count; return mock::call("VENC_ReleaseStream"); }
RK_VOID* RK_MPI_MB_Handle2VirAddr(MB_BLK) { static unsigned char payload[4]{0,0,0,1}; return payload; }
}

int run(bool dual=false)
{
    std::vector<std::string> arguments{"aura-sender-test", "--width", "1280", "--height", "720", "--wrap-lines", "0", "--frames", "3"};
    if (dual) arguments.emplace_back("--dual");
    std::vector<char*> pointers;
    for (auto& arg : arguments) pointers.push_back(arg.data());
    return aura_sender_main(static_cast<int>(pointers.size()), pointers.data());
}
int main()
{
    try {
        for (const auto* phase : {"vi.device", "vi.enabled", "venc.created", "venc.started", "bound", "stream.acquired"}) {
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
        mock::require(mock::cameras[0].gets>4 && mock::cameras[1].gets==4, "completed stream did not continue draining");
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
