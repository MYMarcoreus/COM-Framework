#include "Async/Trace.h"

// ====================================================================
// 异步调用链（trace）的实现 —— 见 Trace.h 的说明
//
// 整段只在 `ASYNC_DEBUG_TRACE`（= 调试构建）下参与编译：发布构建下这里是个空文件
// （没有「空操作版本」——接口本身就不存在）。
//
// 结构很小：一个 thread_local 帧栈 + 顺着每层的上游强引用向上走；走的时候把「视图字段」
// （深度 / 当前层 / 年龄 / 结果）填进那层记录里带出来，所以调用方拿到的一层既有
// 「它是谁」，也有「它跑得怎么样」。
// ====================================================================

#if defined(ASYNC_DEBUG_TRACE)

    #include <atomic>
    #include <chrono>
    #include <cstdio>
    #include <cstring>
    #include <sstream>

    #include "Async/Promise.h"  // 只有这里要看 CPromiseState 的内部（记录读写 / 结果）。

namespace common {
namespace async {

namespace {

/// @brief 距 t0 多久（毫秒）。
///
/// @param t0 起点时刻（steady_clock：只看差值，与绝对时间无关）。
/// @return 毫秒数。
long long MsSince(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

/// @brief 层模式的文本（then / catch / finally）。
///
/// @param eMode 层模式。
/// @return 文本。
const char* ModeText(detail::HandlerMode eMode)
{
    if (eMode == detail::kModeCatch)
    {
        return "catch";
    }
    if (eMode == detail::kModeFinally)
    {
        return "finally";
    }
    return "then";
}

/// @brief 取路径里的文件名部分（打印短一些）。
///
/// @param pszPath 路径（可为空）。
/// @return 文件名（空路径 → `"?"`）。
const char* BaseName(const char* pszPath)
{
    if (pszPath == NULL)
    {
        return "?";
    }
    const char* pszSlash = std::strrchr(pszPath, '/');
    return pszSlash != NULL ? pszSlash + 1 : pszPath;
}

/// @brief 把注册点存的 `__PRETTY_FUNCTION__` 缩成「看得懂的那一段」。
///
/// 注册点存的是 `__PRETTY_FUNCTION__`（精确，但模板实参 / 匿名命名空间全在里面，很长）；
/// 打印时只取最后一个 `::` 之后的函数名，lambda 一律显示成 `<lambda>`。
///
/// @param pszPretty 注册点存的函数名（可为空）。
/// @return 短函数名。
std::string ShortFunc(const char* pszPretty)
{
    if (pszPretty == NULL)
    {
        return "(无注册点)";
    }
    std::string strFunc(pszPretty);
    const size_t nParen = strFunc.find('(');
    if (nParen != std::string::npos)
    {
        strFunc.erase(nParen);  // 去掉参数表
    }
    if (strFunc.find("<lambda") != std::string::npos)
    {
        return "<lambda>";
    }
    const size_t nScope = strFunc.rfind("::");
    if (nScope != std::string::npos)
    {
        strFunc.erase(0, nScope + 2);
    }
    return strFunc;
}

/// @brief 线程 id 的文本（还没跑过 → `"-"`）。
///
/// @param tid 线程 id。
/// @return 文本（libstdc++ 下就是线程号，可拿去和 gdb / htop 里的对）。
std::string ThreadText(const std::thread::id& tid)
{
    if (tid == std::thread::id())
    {
        return "-";
    }
    std::ostringstream oss;
    oss << tid;
    return oss.str();
}

/// @brief 落定结果的文本（还没落定 → 未落定）。
///
/// @param info 一层。
/// @return `「未落定」/「兑现」/「拒绝(码)」`。
std::string ResultText(const CLayerInfo& info)
{
    if (!info.bSettled)
    {
        return "未落定";
    }
    if (info.bFulfilled)
    {
        return "兑现";
    }
    char szBuf[32];
    std::snprintf(szBuf, sizeof(szBuf), "拒绝(%d)", info.nCode);
    return std::string(szBuf);
}

/// @brief 链根标记的文本（空 / `[链根]` / `[子链根]`）。
///
/// @param info 一层。
/// @return 文本。
const char* ChainRootText(const CLayerInfo& info)
{
    if (!info.bChainRoot)
    {
        return "";
    }
    return info.bSubChain ? " [子链根]" : " [链根]";
}

/// @brief 执行器名的文本（未命名 → `-`）。
///
/// @param info 一层。
/// @return 名称文本。
std::string ExecText(const CLayerInfo& info)
{
    return (info.spExecName != nullptr && !info.spExecName->empty()) ? *info.spExecName : std::string("-");
}

/// @brief 填「视图字段」（层状态里不存这些：深度 / 当前层 / 年龄 / 结果）。
///
/// @param info 本层信息（其余字段来自层状态里的那份记录）。
/// @param layer 层状态（取结果用）。
/// @param nSelfMs 本层耗时（当前层：帧算的「已经跑了多久」；上游层：记录里的实际耗时）。
/// @param nDepth 距当前层几跳。
/// @param bCurrent 是不是正在执行的那一层。
void FillView(CLayerInfo& info, const detail::CPromiseState& layer, long long nSelfMs, int nDepth, bool bCurrent)
{
    info.nDepth = nDepth;
    info.bCurrent = bCurrent;
    info.nAgeMs = MsSince(info.tCreated);
    info.nSelfMs = nSelfMs;

    // 上游层的结果：带锁读（trace 会从别的线程看已经落定的上游层）。
    CPromiseResult result;
    info.bSettled = layer.TryGetResult(result);
    if (info.bSettled)
    {
        info.bFulfilled = result.IsFulfilled();
        info.nCode = result.Code();
    }
}

}  // namespace

namespace detail {

/// 层号计数器（从 1 开始；0 表示「没有层号」）。
std::atomic<unsigned> g_nNextLayerId(1);

/// 链号计数器（从 1 开始）。
std::atomic<unsigned> g_nNextChainId(1);

unsigned NextLayerId()
{
    return g_nNextLayerId.fetch_add(1);
}

unsigned NextChainId()
{
    return g_nNextChainId.fetch_add(1);
}

/// 帧栈顶（TLS；帧对象本身活在各线程的栈上，这里只存指针）。
thread_local const CCurrentLayerFrame* g_pTopFrame = nullptr;

CCurrentLayerFrame::CCurrentLayerFrame(const std::shared_ptr<CPromiseState>& spLayer)
    : m_spLayer(spLayer), m_pPrev(g_pTopFrame), m_t0(std::chrono::steady_clock::now())
{
    g_pTopFrame = this;
    if (m_spLayer != nullptr)
    {
        m_spLayer->SetRunningThread(std::this_thread::get_id());  // 「这层跑在哪条线程上」：开跑时写一次。
        // 「这层跑在哪个执行器上」：同一个时机写一次。**只信线程自己的归属** ——
        // 就地层（`ThenInline`）是「接着结算它的那条线程跑」，可能落在别的执行器的线程上，
        // 甚至因为「注册时上游已落定」被投递回本链执行器；究竟落在谁家，只有线程自己知道。
        // 当前线程不属于任何线程池（调用者线程等）→ 记空（打印成 `-`，与 `tid` 一起看）。
        m_spLayer->SetTraceExec(thread::CThreadPool::CurrentPoolName());
    }
}

CCurrentLayerFrame::~CCurrentLayerFrame()
{
    g_pTopFrame = m_pPrev;
}

const CCurrentLayerFrame* CCurrentLayerFrame::Top()
{
    return g_pTopFrame;
}

long long CCurrentLayerFrame::ElapsedMs() const
{
    return MsSince(m_t0);
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
        return g_pTopAdopter->spAdopter;  // 显式指定优先（工厂里现搭的子链 / 协程的子链）。
    }
    const CCurrentLayerFrame* pFrame = CCurrentLayerFrame::Top();
    return (pFrame != nullptr) ? pFrame->LayerState() : std::shared_ptr<CPromiseState>();
}

}  // namespace detail

const CLayerInfo* CurrentLayer()
{
    const detail::CCurrentLayerFrame* pFrame = detail::CCurrentLayerFrame::Top();
    if (pFrame == nullptr || pFrame->Layer() == nullptr)
    {
        return nullptr;
    }

    // TLS 存储：同一线程内「当前层」只有一个，复用一块空间（不分配、无锁）。
    static thread_local CLayerInfo s_info;
    s_info = pFrame->Layer()->LayerInfo();                             // 记录原样带出来
    FillView(s_info, *pFrame->Layer(), pFrame->ElapsedMs(), 0, true);  // 再填视图字段
    return &s_info;
}

bool VisitLayerChain(const std::function<void(const CLayerInfo&)>& fnVisit)
{
    const detail::CCurrentLayerFrame* pFrame = detail::CCurrentLayerFrame::Top();
    if (pFrame == nullptr || pFrame->Layer() == nullptr || !fnVisit)
    {
        return false;
    }

    // 当前层由帧自己保证存活（它正在跑）；往上的每一跳先把上游**升成强引用**再访问
    // （当前层的记录里本来就存着上游的强引用，取出来拿着它去 fetch 下一层即可）。
    const detail::CPromiseState* pLayer = pFrame->Layer();
    CLayerInfo info = pLayer->LayerInfo();
    int nDepth = 0;
    bool bCurrent = true;
    while (true)
    {
        FillView(info, *pLayer, bCurrent ? pFrame->ElapsedMs() : info.nSelfMs, nDepth, bCurrent);
        fnVisit(info);

        if (info.upstream == nullptr)
        {
            break;  // 到链根了
        }
        const std::shared_ptr<detail::CPromiseState> spUpstream = info.upstream;  // 保活到下一轮访问完
        pLayer = spUpstream.get();
        info = pLayer->LayerInfo();
        ++nDepth;
        bCurrent = false;
    }
    return true;
}

std::string DescribeLayerChain()
{
    std::string strChain;
    VisitLayerChain(
        [&strChain](const CLayerInfo& info)
        {
            if (!strChain.empty())
            {
                strChain += " <- ";
            }

            char szBuf[256];
            std::snprintf(szBuf, sizeof(szBuf), "#%d %s %s (%s:%d)", info.nDepth, ModeText(info.eMode),
                info.loc.szFunction != NULL ? info.loc.szFunction : "(无注册点)", info.loc.szFile != NULL ? info.loc.szFile : "?",
                info.loc.nLine);
            strChain += szBuf;
        });
    return strChain;
}

std::string DescribeLayer(const CLayerInfo& info)
{
    // 执行器名连方括号一起补到 12 列（`[main]      `）—— 方括号里不留填充空格，好读。
    std::string strExec = "[" + ExecText(info) + "]";
    if (strExec.size() < 12)
    {
        strExec.append(12 - strExec.size(), ' ');
    }

    char szBuf[512];
    std::snprintf(szBuf, sizeof(szBuf),
        "#%-2d %-7s %-16s %s:%d  %-12.12s 链#%-2u 层#%-3u 龄=%lldms 本层=%lldms 结果=%-8s tid=%-7s%s%s", info.nDepth,
        ModeText(info.eMode), ShortFunc(info.loc.szFunction).c_str(), BaseName(info.loc.szFile), info.loc.nLine, strExec.c_str(),
        info.nChainId, info.nLayerId, info.nAgeMs, info.nSelfMs, ResultText(info).c_str(), ThreadText(info.tid).c_str(),
        ChainRootText(info), info.bCurrent ? "  ← 当前层" : "");
    return std::string(szBuf);
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

}  // namespace async
}  // namespace common

#endif  // defined(ASYNC_DEBUG_TRACE)
