#pragma once
#include <memory>
#include <shared_mutex>
#include <vector>
#include <atomic>
#include <set>
#include <typeindex>
#include <algorithm>
#include <memory_resource>
#include <map>
#include <iostream>
#include <iomanip>
#include <list>

#include "error/located_exception.h"

namespace mempool_util {


	template<size_t i>
	consteval size_t pow2() {
		return 2 * pow2<i - 1>();
	}

	template<>
	consteval size_t pow2<1>() {
		return 2;
	}

	template<>
	consteval size_t pow2<0>() {
		return 1;
	}

	struct pointer_mutex {

		//普通のmutexの方が速かった
		//static thread_local inline uint8_t current_tid = 1;
		//static inline std::atomic_uint8_t counter = 0;
		//
		//struct entry  {
		//	std::atomic_uint8_t mtx = 0;
		//
		//	void lock() {
		//
		//		while (true)
		//		{
		//			uint8_t temp = 0;
		//			if (mtx.compare_exchange_weak(temp, current_tid))
		//				break;
		//			mtx.wait(temp);
		//		}
		//
		//	}
		//
		//	void unlock() {
		//		mtx.store(0);
		//		mtx.notify_one();
		//	}
		//};


		static inline constexpr size_t size = mempool_util::pow2<10>();
		static inline constexpr size_t mask = size - 1;

		std::mutex arr[size];

		static inline size_t make_hash(void* ptr)noexcept {
			return (uintptr_t)ptr;//shiftしない方が速かった
		}

		static void init_thread() {
			//current_tid = ++counter;
		}

		void lock(void* ptr) {
			arr[make_hash(ptr) & mask].lock();
		}
		void unlock(void* ptr) {
			arr[make_hash(ptr) & mask].unlock();
		}
	};
	pointer_mutex& get_pointer_mutex();
};




struct mt_mempool {

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

			auto ns =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					end - _begin).count();

			std::cout
				<< std::setw(32)
				<< std::left
				<< _name
				<< " : "
				<< ns
				<< " ns"
				<< std::endl;
		}
	};

	static constexpr size_t chunk_size = 144 * 100000;

	struct inst_info {
		char* ptr = nullptr;//オブジェクトが格納されたptrを指す 必ず先頭に
		//std::type_index tid = typeid(void);//voidの場合削除済みを示す
		//size_t tid = 0;//tmapのindexを指す

		char* (*move)(char*&, char*) = nullptr;

	};

	struct allocator {
		mutable std::mutex inst_mtx;
		mutable std::mutex allo_deallo_mtx;
		std::pmr::unsynchronized_pool_resource inst_pool;
		mempool_util::pointer_mutex* ptr_mtx;

		size_t back_ind = 0;
		char data[chunk_size];
		std::pmr::list<inst_info> inst_list;

		allocator() : inst_list(&inst_pool){
			ptr_mtx = &mempool_util::get_pointer_mutex();
		}

		~allocator() {

		}

		template<typename t>
		bool allocateable() const noexcept{
			std::unique_lock<std::mutex> lk(allo_deallo_mtx, std::try_to_lock);
			return lk && (back_ind + (uintptr_t)&data[back_ind] % alignof(t)) + sizeof(t) < chunk_size;
		}

		template<typename t>
		t** construct() {
			inst_info* info_ptr;

			{
				std::lock_guard lock(inst_mtx);
				info_ptr = &inst_list.emplace_back();
				info_ptr->move = &move<t>;
			}


			{
				std::lock_guard lock(allo_deallo_mtx);
				//次の取得アドレスがアラインに沿っているか
				back_ind += (uintptr_t)&data[back_ind] % alignof(t);
				info_ptr->ptr = &data[back_ind];//info更新　この時点でデストラクタは呼ばれている必要がある
				back_ind += sizeof(t);
			}


			if constexpr (!std::is_trivially_copyable_v<t>) {
				new (info_ptr->ptr) t();
			}
			return reinterpret_cast<t**>(reinterpret_cast<char*>(info_ptr));
		};

		void erase_inst(inst_info* ptr) {
			ptr_mtx->lock(ptr);
			ptr->move = nullptr;
			ptr_mtx->unlock(ptr);
		}


		void gc() {

			decltype(inst_list.end()) end;
			decltype(inst_list.end()) i;

			{
				std::lock_guard lock(inst_mtx);
				i = inst_list.begin();
				end = inst_list.end();
			}
			char* next = data;

			
			for(; i != end;)
			{
				ptr_mtx->lock(&i);

				if (!i->move) {
					ptr_mtx->unlock(&i);
					i = inst_list.erase(i);

					continue;
				}

				{
					auto src = i->ptr;
					next = i->move(i->ptr, src);
				}

				ptr_mtx->unlock(&i);
				
				++i;
			}

			std::lock_guard lock(allo_deallo_mtx);
			back_ind = (next - &data[0]);
		}

	};

	template<typename t>
	struct inst_deleter {
		allocator* owner = nullptr;
		inst_deleter(allocator* owner) : owner(owner) {};
		inst_deleter(const inst_deleter& other) = default;
		inst_deleter(inst_deleter&& other) = default;

		void operator()(void* block)const {
			static_assert(std::is_standard_layout_v<inst_info>, "ポインタ変換が行えない環境");
			if constexpr (!std::is_trivially_copyable_v<t>) {//トリビアル型以外はデストラクト	
				reinterpret_cast<t*>(block)->~t();
			}
			owner->erase_inst(reinterpret_cast<inst_info*>(block));
		}
	};


	/// <summary>
	/// 
	/// 指定位置にオブジェクトを構築する
	/// 次のオブジェクトアドレスを返す
	/// 
	/// dstはアラインメント調整して返される為注意
	/// </summary>
	/// <typeparam name="t"></typeparam>
	/// <param name="dst"></param>
	/// <param name="src"></param>
	/// <param name="size"></param>
	/// <returns></returns>
	template<typename t>
	static char* move(char*& dst,char* src) {

		dst += ((uintptr_t)dst % alignof(t));//アラインメント詰めのアドレスにする

		if constexpr (std::is_trivially_copyable_v<t>) {
			std::memmove(dst, src, sizeof(t));
		}
		else {
#ifdef _DEBUG
			if (dst > src) {//dstの方が前であるべき
				throw error::located_exception("fetal error invalid object move");
			}
#endif
			//アドレス距離がオブジェクトのサイズ未満だった場合、移動先と移動元は重なっている

			if ((dst - src) < sizeof(t)) {//間が１要素以上あるならば問題はない為この境界
				t temp(std::move(
					*reinterpret_cast<t*>(src)
				));
				reinterpret_cast<t*>(src)->~t();
				new (reinterpret_cast<t*>(dst)) t(std::move(temp));
			}
			else {
				new (reinterpret_cast<t*>(dst)) t(
					std::move(*reinterpret_cast<t*>(src))
				);
				reinterpret_cast<t*>(src)->~t();
			}
		}
		return dst + sizeof(t);
	}

	std::mutex mtx;
	std::vector<std::unique_ptr<allocator>> chunk_arr;

	mt_mempool() {

		if (chunk_arr.empty()) {
			chunk_arr.emplace_back(std::make_unique<allocator>());
		}
	}


	template<typename t>
	std::unique_ptr<t*,inst_deleter<t>> construct() {

		{
			for (auto& chunk : chunk_arr)
			{
				if (chunk->allocateable<t>()) {
					return std::unique_ptr<t*, inst_deleter<t>>(
						chunk->construct<t>(),
						inst_deleter<t>(chunk.get())
					);
				}
			}
		}

		allocator* owner = chunk_arr.emplace_back(std::make_unique<allocator>()).get();

		return std::unique_ptr<t*, inst_deleter<t>>(
			owner->construct<t>(),
			inst_deleter<t>(owner)
		);
	}
};


template<typename t>
struct accessor {
	mempool_util::pointer_mutex& mtx;

	accessor() : mtx(mempool_util::get_pointer_mutex()) {}
	t& operator()(void* info) {
		return *(t*)((mt_mempool::inst_info*)info)->ptr;
	}

	void lock(const std::unique_ptr<t*,mt_mempool::inst_deleter<t>>& ptr) {
		mtx.lock(ptr.get());
	}

	void unlock(const std::unique_ptr<t*, mt_mempool::inst_deleter<t>>& ptr) {
		mtx.unlock(ptr.get());
	}
};




