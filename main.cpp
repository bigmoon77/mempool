#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>



#include <iostream>
#include <string>
#include <cassert>
#include <vector>
#include "mempool.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <string>

#include "mempool.h"

//
// テスト用型
//

struct lifecheck {

    static inline int ctor_count = 0;
    static inline int dtor_count = 0;
    static inline int move_count = 0;

    int value = 0;

    lifecheck(int v = 0)
        : value(v)
    {
        ++ctor_count;
    }

    lifecheck(const lifecheck&) = delete;
    lifecheck& operator=(const lifecheck&) = delete;

    lifecheck(lifecheck&& other) noexcept {
        value = other.value;
        ++ctor_count;
        ++move_count;
    }

    lifecheck& operator=(lifecheck&& other) noexcept {
        value = other.value;
        ++move_count;
        return *this;
    }

    ~lifecheck() {
        ++dtor_count;
    }

    static void reset() {
        ctor_count = 0;
        dtor_count = 0;
        move_count = 0;
    }
};


//
// メモリアドレスの詰め確認用
//

struct packed_data {

    int id = 0;
    std::string str;

    packed_data(int i = 0)
        : id(i), str("test")
    {
    }

    packed_data(const packed_data&) = delete;
    packed_data& operator=(const packed_data&) = delete;

    packed_data(packed_data&& other) noexcept
        : id(other.id), str(std::move(other.str))
    {
        
    }

    ~packed_data() = default;
};


//
// leak check
//

void test_leak_check() {

    std::cout << "===== leak check =====\n";

    lifecheck::reset();

    {
        anytype_pool<1024> pool;

        auto a = pool.construct_unique<lifecheck>(1);
        auto b = pool.construct_unique<lifecheck>(2);

        pool.destruct(a);
        pool.destruct(b);
    }

    std::cout << "ctor : " << lifecheck::ctor_count << "\n";
    std::cout << "dtor : " << lifecheck::dtor_count << "\n";

    assert(lifecheck::ctor_count == lifecheck::dtor_count);

    std::cout << "ok\n\n";
}

//
// GCでメモリが詰められているか
//

void test_gc_compaction() {

    std::cout << "===== gc compaction =====\n";

    anytype_pool<256>::chunk<256> chunk;

    auto a = chunk.construct_unique<packed_data>(1);
    auto b = chunk.construct_unique<packed_data>(2);
    auto c = chunk.construct_unique<packed_data>(3);

    auto pa = *a.get();
    auto pb = *b.get();
    auto pc = *c.get();

    std::cout << "before gc\n";
    std::cout << "a : " << pa << "\n";
    std::cout << "b : " << pb << "\n";
    std::cout << "c : " << pc << "\n";

    //
    // 真ん中だけ解放
    //

    chunk.destruct(b);

    chunk.gc();

    auto new_pa = *a.get();
    auto new_pc = *c.get();

    std::cout << "\nafter gc\n";
    std::cout << "a : " << new_pa << "\n";
    std::cout << "c : " << new_pc << "\n";

    //
    // c が b の位置へ前詰めされているか
    //

    assert(new_pa == pa);

    assert(
        reinterpret_cast<std::uintptr_t>(new_pc)
        ==
        reinterpret_cast<std::uintptr_t>(new_pa) + sizeof(packed_data)
    );

    assert(new_pc->id == 3);

    std::cout << "ok\n\n";
}

//
// dead space確認
//

void test_dead_space() {

    std::cout << "===== dead space =====\n";

    anytype_pool<256>::chunk<256> chunk;

    auto a = chunk.construct_unique<lifecheck>(1);
    auto b = chunk.construct_unique<lifecheck>(2);

    size_t before = chunk.dead_space();

    chunk.destruct(a);

    size_t after = chunk.dead_space();

    std::cout << "before : " << before << "\n";
    std::cout << "after  : " << after << "\n";

    assert(after >= sizeof(lifecheck));

    chunk.gc();

    assert(chunk.dead_space() == 0);

    std::cout << "ok\n\n";
}

//
// 配列版テスト
//

void test_array_pool() {

    std::cout << "===== array pool =====\n";

    arraysup_anytype_pool<1024> pool;

    auto arr = pool.construct_unique<lifecheck>(10);

    for (size_t i = 0; i < 10; i++)
    {
        (*arr.get())[i].value = static_cast<int>(i);
    }

    pool.gc();

    for (size_t i = 0; i < 10; i++)
    {
        assert((*arr.get())[i].value == static_cast<int>(i));
    }

    pool.destruct(arr);

    std::cout << "ok\n\n";
}

//
// shared_ptr test
//

void test_shared() {

    std::cout << "===== shared ptr =====\n";

    anytype_pool<1024> pool;

    auto a = pool.construct_shared<packed_data>(123);

    {
        auto b = a;

        assert(a.use_count() == 2);
    }

    assert(a.use_count() == 1);

    pool.gc();

    assert((*a.get())->id == 123);

    std::cout << "ok\n\n";
}

//
// 大量生成とgc
//

void stress_test() {

    std::cout << "===== stress test =====\n";

    anytype_pool<4096> pool;

    std::vector<anytype_pool<4096>::unique_ptr_type<packed_data>> objs;

    for (int i = 0; i < 100; i++)
    {
        objs.emplace_back(
            pool.construct_unique<packed_data>(i)
        );
    }

    //
    // 半分解放
    //

    for (size_t i = 0; i < objs.size(); i += 2)
    {
        pool.destruct(objs[i]);
    }

    pool.gc();

    //
    // 生存確認
    //

    for (size_t i = 1; i < objs.size(); i += 2)
    {
        assert((*objs[i].get())->id == static_cast<int>(i));
    }

    std::cout << "ok\n\n";
}

int test_pool() {

    test_leak_check();

    test_gc_compaction();

    test_dead_space();

    test_array_pool();

    test_shared();

    stress_test();

    std::cout << "=========================\n";
    std::cout << "all test passed\n";
    std::cout << "=========================\n";

    return 0;
}

#include <iostream>
#include <vector>
#include <chrono>
#include <memory_resource>
#include <random>
#include <cstring>
#include <iomanip>

#include "mempool.h" // mem::anytype_pool を定義したヘッダ

// ------------------------------------------------------------
// ベンチ対象オブジェクト
// ------------------------------------------------------------

struct test_object {

    int id = 0;
    float pos[3]{};
    char buffer[128]{};

    test_object() {
        std::memset(buffer, 1, sizeof(buffer));
    }

    ~test_object() {
        // destructor cost simulation
        buffer[0] = 0;
    }
};

// ------------------------------------------------------------
// タイマー
// ------------------------------------------------------------

class scoped_timer {
    using clock = std::chrono::high_resolution_clock;

    std::string _name;
    clock::time_point _begin;

public:
    scoped_timer(std::string name)
        : _name(std::move(name)), _begin(clock::now()) {
    }

    ~scoped_timer() {
        auto end = clock::now();

        auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - _begin).count();

        std::cout
            << std::setw(32)
            << std::left
            << _name
            << " : "
            << us
            << " us"
            << std::endl;
    }
};

// ------------------------------------------------------------
// メモリ使用量推定
// ------------------------------------------------------------

template<typename T>
size_t estimate_memory_usage(size_t count) {
    return sizeof(T) * count;
}

void benchmark_new_delete(
    size_t object_count,
    size_t loop_count
) {

    std::cout
        << "\n===== new / delete =====\n";

    {
        scoped_timer timer("construct + destruct");

        for (size_t loop = 0; loop < loop_count; loop++)
        {
            std::vector<test_object*> ptrs;
            ptrs.reserve(object_count);
            {
                // construct
                for (size_t i = 0; i < object_count; i++)
                {
                    ptrs.emplace_back(
                        new test_object()
                    );
                }
            }

            // destruct
            for (auto ptr : ptrs)
            {
                delete ptr;
            }
        }
    }

    {
        scoped_timer timer("construct only");

        std::vector<test_object*> ptrs;
        ptrs.reserve(object_count);

        for (size_t i = 0; i < object_count; i++)
        {
            ptrs.emplace_back(
                new test_object()
            );
        }

        for (auto ptr : ptrs)
        {
            delete ptr;
        }
    }

    std::cout
        << "estimated raw memory : "
        << estimate_memory_usage<test_object>(object_count)
        << " bytes\n";
}

void benchmark_mem_pool(
    size_t object_count,
    size_t loop_count
) {

    using pool_type = mempool;

    std::cout << "\n===== mem pool =====\n";

    pool_type pool;

    {
        scoped_timer timer("construct + destruct");

        for (size_t loop = 0; loop < loop_count; loop++)
        {
            std::vector<std::unique_ptr<test_object, mempool::deleter<test_object>>> ptrs;
            ptrs.reserve(object_count);

            {
                //scoped_timer timer("construct");

                // construct
                for (size_t i = 0; i < object_count; i++)
                {
                    ptrs.emplace_back(
                        pool.construct_uni<test_object>()
                    );
                }
            }
        }
    }

    {
        scoped_timer timer("construct only");

        std::vector<std::unique_ptr<test_object,mempool::deleter<test_object>>> ptrs;
        ptrs.reserve(object_count);

        for (size_t i = 0; i < object_count; i++)
        {
            ptrs.emplace_back(
                pool.construct_uni<test_object>()
            );
        }
    }

    std::cout
        << "estimated raw memory : "
        << estimate_memory_usage<test_object>(object_count)
        << " bytes\n";
}

void benchmark_custom_pool(
    size_t object_count,
    size_t loop_count
) {

    using pool_type = arraysup_anytype_pool<144 * 100000>;

    std::cout << "\n===== gc mem pool =====\n";

    pool_type pool;

    {
        scoped_timer timer("construct + destruct");

        for (size_t loop = 0; loop < loop_count; loop++)
        {
            std::vector<pool_type::unique_ptr_type<test_object>> ptrs;
            ptrs.reserve(object_count);

            {
                //scoped_timer timer("construct");

                // construct
                for (size_t i = 0; i < object_count; i++)
                {
                    ptrs.emplace_back(
                        pool.construct_unique<test_object>(1)
                    );
                }
            }
        }
    }

    {
        scoped_timer timer("construct only");

        std::vector<pool_type::unique_ptr_type<test_object>> ptrs;
        ptrs.reserve(object_count);

        for (size_t i = 0; i < object_count; i++)
        {
            ptrs.emplace_back(
                pool.construct_unique<test_object>(1)
            );
        }
    }

    std::cout
        << "estimated raw memory : "
        << estimate_memory_usage<test_object>(object_count)
        << " bytes\n";
}

// ------------------------------------------------------------
// std::pmr benchmark
// ------------------------------------------------------------

void benchmark_pmr_pool(
    size_t object_count,
    size_t loop_count
) {

    std::cout << "\n===== std::pmr::unsynchronized_pool_resource =====\n";

    {
        scoped_timer timer("construct + destruct");

        for (size_t loop = 0; loop < loop_count; loop++)
        {
            std::pmr::unsynchronized_pool_resource pool;

            std::pmr::polymorphic_allocator<test_object> alloc(&pool);

            std::vector<test_object*> ptrs;
            ptrs.reserve(object_count);

            {
                //scoped_timer timer("construct");

                // construct
                for (size_t i = 0; i < object_count; i++)
                {
                    auto ptr = alloc.allocate(1);

                    std::construct_at(ptr);
                    ptrs.emplace_back(ptr);
                }
            }

            // destruct
            for (auto ptr : ptrs)
            {
                std::destroy_at(ptr);
                alloc.deallocate(ptr, 1);
            }
        }
    }

    {
        scoped_timer timer("construct only");

        std::pmr::unsynchronized_pool_resource pool;

        std::pmr::polymorphic_allocator<test_object> alloc(&pool);

        std::vector<test_object*> ptrs;
        ptrs.reserve(object_count);

        for (size_t i = 0; i < object_count; i++)
        {
            auto ptr = alloc.allocate(1);

            std::construct_at(ptr);

            ptrs.emplace_back(ptr);
        }

        for (auto ptr : ptrs)
        {
            std::destroy_at(ptr);
            alloc.deallocate(ptr, 1);
        }
    }

    std::cout
        << "estimated raw memory : "
        << estimate_memory_usage<test_object>(object_count)
        << " bytes\n";
}

// ------------------------------------------------------------
// monotonic benchmark
// ------------------------------------------------------------

void benchmark_monotonic_pool(
    size_t object_count,
    size_t loop_count
) {

    std::cout << "\n===== std::pmr::monotonic_buffer_resource =====\n";

    constexpr size_t buffer_size =
        1024ull * 1024ull * 64ull;

    std::vector<std::byte> backing(buffer_size);

    {
        scoped_timer timer("construct + bulk release");

        for (size_t loop = 0; loop < loop_count; loop++)
        {
            std::pmr::monotonic_buffer_resource pool(
                backing.data(),
                backing.size()
            );

            std::pmr::polymorphic_allocator<test_object> alloc(&pool);

            std::vector<test_object*> ptrs;
            ptrs.reserve(object_count);

            {

                //scoped_timer timer("construct");
                for (size_t i = 0; i < object_count; i++)
                {
                    auto ptr = alloc.allocate(1);

                    std::construct_at(ptr);

                    ptrs.emplace_back(ptr);
                }
            }

            for (auto ptr : ptrs)
            {
                std::destroy_at(ptr);
            }

            // monotonic は個別解放なし
        }
    }

    std::cout
        << "backing memory : "
        << buffer_size
        << " bytes\n";
}


int main() {
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    test_pool();



    {

        constexpr size_t object_count = 100000;
        constexpr size_t loop_count = 5;

        std::cout
            << "object size : "
            << sizeof(test_object)
            << " bytes\n";

        std::cout
            << "object count : "
            << object_count
            << "\n";

        std::cout
            << "loop count : "
            << loop_count
            << "\n";
        benchmark_new_delete(
            object_count,
            loop_count
        );
        std::this_thread::sleep_for(std::chrono::seconds(5));
        benchmark_mem_pool(
            object_count,
            loop_count
        );
        std::this_thread::sleep_for(std::chrono::seconds(5));
        benchmark_custom_pool(
            object_count,
            loop_count
        );

        std::this_thread::sleep_for(std::chrono::seconds(5));
        benchmark_pmr_pool(
            object_count,
            loop_count
        );

        std::this_thread::sleep_for(std::chrono::seconds(5));
        benchmark_monotonic_pool(
            object_count,
            loop_count
        );
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}