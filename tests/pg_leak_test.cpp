// 验证：Release 下去掉 #ifdef _DEBUG 调用 _Crt* 是否有效
#include <winsock2.h>
#include <libpq-fe.h>
#include <crtdbg.h>
#include <cstdio>
#include <thread>

static void worker() {
    PGconn* c = PQconnectdb("host=127.0.0.1 port=5432 user=postgres password=postgres dbname=hermes_test sslmode=disable");
    if (PQstatus(c) != CONNECTION_OK) {
        printf("CONNECT FAILED: %s\n", PQerrorMessage(c));
        PQfinish(c);
        return;
    }
    printf("CONNECT OK\n");

    PGresult* r1 = PQexecParams(c, "SELECT 1", 0, NULL, NULL, NULL, NULL, 0);
    printf("SELECT 1: %s\n", PQresultStatus(r1) == PGRES_TUPLES_OK ? "OK" : "FAIL");
    PQclear(r1);

    const char* vals[] = {"db-ping"};
    PGresult* r2 = PQexecParams(c, "INSERT INTO hermes_selfcheck (v) VALUES ($1)", 1, NULL, vals, NULL, NULL, 0);
    printf("INSERT: %s\n", PQresultStatus(r2) == PGRES_COMMAND_OK ? "OK" : "FAIL");
    PQclear(r2);

    PGresult* r3 = PQexec(c, "SELECT id, v FROM hermes_selfcheck ORDER BY id DESC LIMIT 1");
    printf("SELECT rows: %s\n", PQresultStatus(r3) == PGRES_TUPLES_OK ? "OK" : "FAIL");
    PQclear(r3);

    PQfinish(c);
    printf("ALL DONE\n");
}

int main(void) {
    // 故意不包 #ifdef _DEBUG —— Release 下也调用（验证是否为 no-op）
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);

    std::thread t(worker);
    t.join();

    printf("--- manual dump ---\n");
    int leaked = _CrtDumpMemoryLeaks();
    printf("--- done, _CrtDumpMemoryLeaks returned %d ---\n", leaked);
    return 0;
}
