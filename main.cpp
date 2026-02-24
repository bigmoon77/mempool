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

    anytype_pool<100> c;

    std::cout << "==== trivial type test ====\n";
    {
        auto p = c.construct<int,int>(0,5);
        for (size_t i = 0; i < 5; ++i)
            std::cout << p[i] << " ";

        for (size_t i = 0; i < 5; ++i)
            p[i] = static_cast<int>(i * 10);
        std::cout << " => ";
        for (size_t i = 0; i < 5; ++i)
            std::cout << p[i] << " ";
        std::cout << "\n";

        c.destruct(p);//ここでデストロイしないとgc時にメモリが動かない為テストにならない 都度変更
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
    std::cout << "==== end main ====\n";
}