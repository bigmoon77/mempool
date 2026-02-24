#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>



#include <iostream>
#include <string>
#include <cassert>
#include "mempool.h"

// chunk の定義はここに貼る

// =====================
// 非トリビアル型テスト用
// =====================
struct test_obj {
    int value;

    test_obj() : value(0) {
        std::cout << "ctor\n";
    }

    test_obj(int v) : value(v) {
        std::cout << "ctor(int)\n";
    }

    test_obj(test_obj&& other) noexcept {
        std::cout << "move ctor\n";
        value = other.value;
        other.value = -1;
    }

    ~test_obj() {
        std::cout << "dtor : " << value << "\n";
    }
};

int main() {
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);






    std::cout << "==== create pool ====\n";
    unitype_pool<test_obj, 16> pool;

    std::cout << "\n==== single construct ====\n";
    auto p1 = pool.construct(10);
    std::cout << "value: " << p1->value << "\n";
    std::cout << "use_count: " << p1.get_use_count() << "\n";

    std::cout << "\n==== copy proxy ====\n";
    {
        auto p2 = p1;
        std::cout << "use_count after copy: " << p1.get_use_count() << "\n";
    }
    std::cout << "use_count after scope exit: " << p1.get_use_count() << "\n";

    std::cout << "\n==== array construct ====\n";
    auto arr = pool.construct(3, 5); // value=5, num=3
    for (size_t i = 0; i < arr.get_size(); ++i) {
        std::cout << arr[i].value << " ";
    }
    std::cout << "\n";

    std::cout << "\n==== destruct test ====\n";
    pool.destruct(p1);

    std::cout << "\n==== gc test ====\n";
    pool.gc();

    std::cout << "\n==== create more objects ====\n";
    auto p3 = pool.construct(42);
    auto p4 = pool.construct(100);

    std::cout << "p3: " << p3->value << "\n";
    std::cout << "p4: " << p4->value << "\n";

    std::cout << "\n==== force fragmentation ====\n";
    pool.destruct(p3);
    pool.gc();

    std::cout << "\n==== mem efficiency check ====\n";
    pool.gc();

    std::cout << "\n==== end ====\n";





    anytype_pool<100> c;

    std::cout << "==== trivial type test ====\n";
    {
        auto p = c.construct<int>(5);

        for (size_t i = 0; i < 5; ++i)
            p[i] = static_cast<int>(i * 10);
        std::cout << " => ";

        for (size_t i = 0; i < 5; ++i)
            std::cout << p[i] << " ";
        std::cout << "\n";
    }

    std::cout << "==== non trivial type test ====\n";
    {
        auto p = c.construct<test_obj>(3);

        p[0].value = 10;
        p[1].value = 20;
        p[2].value = 30;

        std::cout << "before gc\n";
        c.gc();
        std::cout << "after gc\n";

        std::cout << p[0].value << " "
            << p[1].value << " "
            << p[2].value << "\n";
    }

    std::cout << "==== destroy test ====\n";
    {
        auto p1 = c.construct<test_obj>(2);
        auto p2 = c.construct<int>(4);

        c.destruct(p1);

        std::cout << "gc after destroy\n";
        c.gc();

        for (size_t i = 0; i < p2.get_size(); ++i)
            std::cout << p2[i] << " ";
        std::cout << "\n";
    }
    c.gc();
    std::cout << "==== end main ====\n";
}