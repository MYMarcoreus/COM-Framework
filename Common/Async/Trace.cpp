#include "Async/Trace.h"

#include <cstdio>

#include "Async/Promise.h"  // 只有这里要看 CPromiseState 的内部（Loc / Mode / Upstream）。

// ====================================================================
// 异步调用链（trace）的实现 —— 见 Trace.h 的说明
//
// 结构很小：一个 thread_local 帧栈 + 顺着每层的上游指针（强引用，注册时设一次）向上走。
// 全部在 `ASYNC_DEBUG_TRACE`（= 调试构建）下生效，发布构建编译成空操作。
// ====================================================================

namespace common {
namespace async {

namespace detail {

#if defined(ASYNC_DEBUG_TRACE)

/// 帧栈顶（TLS；帧对象本身活在各线程的栈上，这里只存指针）。
thread_local const CCurrentLayerFrame* g_pTopFrame = nullptr;

CCurrentLayerFrame::CCurrentLayerFrame(const std::shared_ptr<CPromiseState>& spLayer) : m_spLayer(spLayer), m_pPrev(g_pTopFrame)
{
    g_pTopFrame = this;
}

CCurrentLayerFrame::~CCurrentLayerFrame()
{
    g_pTopFrame = m_pPrev;
}

const CCurrentLayerFrame* CCurrentLayerFrame::Top()
{
    return g_pTopFrame;
}

/// 起链父层的作用域栈顶（TLS；节点活在各自栈上）。
thread_local const CChainAdopterScope::CNode* g_pTopAdopter = nullptr;

CChainAdopterScope::CChainAdopterScope(const std::shared_ptr<CPromiseState>& spAdopter) : m_node{spAdopter, g_pTopAdopter}
{
    g_pTopAdopter = &m_node;
}

CChainAdopterScope::~CChainAdopterScope()
{
    g_pTopAdopter = m_node.pPrev;
}

std::shared_ptr<CPromiseState> CurrentLayerState()
{
    if (g_pTopAdopter != nullptr)
    {
        return g_pTopAdopter->spAdopter;  // 显式指定优先（工厂里现搭的子链）。
    }
    const CCurrentLayerFrame* pFrame = CCurrentLayerFrame::Top();
    return (pFrame != nullptr) ? pFrame->LayerState() : std::shared_ptr<CPromiseState>();
}

#else

CCurrentLayerFrame::CCurrentLayerFrame(const std::shared_ptr<CPromiseState>& spLayer) : m_spLayer(spLayer), m_pPrev(nullptr)
{}

CCurrentLayerFrame::~CCurrentLayerFrame()
{}

const CCurrentLayerFrame* CCurrentLayerFrame::Top()
{
    return nullptr;
}

CChainAdopterScope::CChainAdopterScope(const std::shared_ptr<CPromiseState>& spAdopter) : m_node{spAdopter, nullptr}
{}

CChainAdopterScope::~CChainAdopterScope()
{}

std::shared_ptr<CPromiseState> CurrentLayerState()
{
    return std::shared_ptr<CPromiseState>();
}

#endif

}  // namespace detail

#if defined(ASYNC_DEBUG_TRACE)

const CLayerInfo* CurrentLayer()
{
    const detail::CCurrentLayerFrame* pFrame = detail::CCurrentLayerFrame::Top();
    if (pFrame == nullptr || pFrame->Layer() == nullptr)
    {
        return nullptr;
    }

    // TLS 存储：同一线程内「当前层」只有一个，复用一块空间（不分配、无锁）。
    static thread_local CLayerInfo s_info;
    s_info.loc = pFrame->Layer()->Loc();
    s_info.eMode = pFrame->Layer()->Mode();
    s_info.nDepth = 0;
    s_info.bCurrent = true;
    return &s_info;
}

bool VisitLayerChain(const std::function<void(const CLayerInfo&)>& fnVisit)
{
    const detail::CCurrentLayerFrame* pFrame = detail::CCurrentLayerFrame::Top();
    if (pFrame == nullptr || pFrame->Layer() == nullptr || !fnVisit)
    {
        return false;
    }

    CLayerInfo info;
    info.bCurrent = true;

    // 当前层由帧自己保证存活（它正在跑）；往上的每一跳要**先升成强引用再访问** ——
    // 否则「拿到裸指针 → 上一跳的强引用析构 → 节点被释放」就会悬垂。
    const detail::CPromiseState* pLayer = pFrame->Layer();
    std::shared_ptr<detail::CPromiseState> spKeepAlive;
    while (pLayer != nullptr)
    {
        info.loc = pLayer->Loc();
        info.eMode = pLayer->Mode();
        fnVisit(info);

        ++info.nDepth;
        info.bCurrent = false;

        const std::shared_ptr<detail::CPromiseState> spUpstream = pLayer->Upstream();
        spKeepAlive = spUpstream;   // 保活到下一轮访问完
        pLayer = spUpstream.get();  // 空 → 到链根了
    }
    return true;
}

std::string DescribeLayerChain()
{
    std::string strChain;
    VisitLayerChain(
        [&strChain](const CLayerInfo& info)
        {
            const char* pszMode = "then";
            if (info.eMode == detail::kModeCatch)
            {
                pszMode = "catch";
            }
            else if (info.eMode == detail::kModeFinally)
            {
                pszMode = "finally";
            }
            if (!strChain.empty())
            {
                strChain += " <- ";
            }

            char szBuf[256];
            std::snprintf(szBuf, sizeof(szBuf), "#%d %s %s (%s:%d)", info.nDepth, pszMode,
                info.loc.szFunction != NULL ? info.loc.szFunction : "(无注册点)", info.loc.szFile != NULL ? info.loc.szFile : "?",
                info.loc.nLine);
            strChain += szBuf;
        });
    return strChain;
}

void DumpLayerChain()
{
    const std::string strChain = DescribeLayerChain();
    if (strChain.empty())
    {
        std::fprintf(stderr, "[async 链] (当前不在任何层处理器里)\n");
        return;
    }
    std::fprintf(stderr, "[async 链] %s\n", strChain.c_str());
}

#else

const CLayerInfo* CurrentLayer()
{
    return nullptr;  // 发布构建：trace 是空操作（与 ASYNC_LOC 同一个开关）。
}

bool VisitLayerChain(const std::function<void(const CLayerInfo&)>& fnVisit)
{
    (void)fnVisit;
    return false;
}

std::string DescribeLayerChain()
{
    return std::string();
}

void DumpLayerChain()
{}

#endif

}  // namespace async
}  // namespace common
