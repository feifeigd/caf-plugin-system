// crt_recalloc_repro.c — 最小复现 PG 51 块的来源：_recalloc_dbg 128B 块
// 结论：PG 排查中每连接 51 块的 128B 链表节点池 = _recalloc_dbg 分配的
//       reachable 缓存（分配后全局持有不释放），非插件/libpq 泄露。
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>

int main(void) {
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);

    /* 模拟 UCRT 惰性缓存：_recalloc_dbg(1,128) 分配 128B 后全局持有（不释放）。
       与 PG 连接时 ucrtbased.dll+0x7fbe5 处 128B 块同一分配函数、同一大小。 */
    void* p = _recalloc_dbg(NULL, 1, 128, _NORMAL_BLOCK, __FILE__, __LINE__);
    (void)p;

    _CrtDumpMemoryLeaks();
    return 0;
}
