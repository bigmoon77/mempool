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
#include <deque>

#include <cassert>

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
		char* ptr;//オブジェクトが格納されたptrを指す 必ず先頭に
		//std::type_index tid = typeid(void);//voidの場合削除済みを示す
		//size_t tid = 0;//tmapのindexを指す
		char* (*move)(char*&, char*, bool);

		//最初はnull以外の適当な値を入れておく
		inst_info* next_free;

		bool operator < (const inst_info& other)const {
			return ptr < other.ptr;
		}
		bool operator == (const inst_info& other)const {
			return ptr == other.ptr;
		}
	};

	struct append_only_deque {

		static inline constexpr size_t chunk_size = mempool_util::pow2<10>();

		using value_type = inst_info;

		struct chunk {
			char data[sizeof(value_type) * chunk_size];
		};

		std::vector<chunk*> chunk_arr;

		size_t back_ind = 0;
		struct iterator {
			size_t cind = 0;
			size_t ind = 0;
			decltype(chunk_arr) arr;

			iterator(size_t cind, size_t ind, decltype(chunk_arr)& arr)
				:cind(cind), ind(ind), arr(arr) {

			}

			iterator() = default;

			value_type* operator ->() {
				return
					&reinterpret_cast<value_type*>(arr[cind]->data)[ind];
			}

			value_type& operator*() {
				return
					reinterpret_cast<value_type*>(arr[cind]->data)[ind];
			}

			void operator ++() {
				++ind;
				ind = (ind != chunk_size) * ind;//加算してサイズ上限の場合0
				cind += (ind == 0);//先にindを処理するのでこの時点で0の場合、チャンク入れ代わり直後
			}

			bool operator != (const iterator& other)const {
				return std::memcmp(this, &other, sizeof(size_t) * 2);
			}
		};

		~append_only_deque() {
			for (auto& i : chunk_arr)
			{
				delete i;
			}
		}

		value_type& emplace_back() {
			auto ind = back_ind % chunk_size;

			if (ind) {
				value_type& res = reinterpret_cast<value_type*>(
					chunk_arr.back()->data
					)[ind];

				++back_ind;
				//back_ind %= chunk_size;
				return res;
			}

			chunk_arr.emplace_back(new chunk);
			value_type& res = reinterpret_cast<value_type*>(
				chunk_arr.back()->data
				)[ind];

			++back_ind;
			//back_ind %= chunk_size;
			return res;
		}

		iterator begin() {
			return iterator(0, 0, chunk_arr);
		}
		iterator end() {

			if (back_ind % chunk_size == 0){

				return iterator(chunk_arr.size(),
					0,
					chunk_arr);
			}
			else {

				return iterator(chunk_arr.size() - 1,
					back_ind % chunk_size,
					chunk_arr);
			}
		}
	};

	struct allocator {
		mutable std::mutex inst_mtx;
		mutable std::mutex allo_deallo_mtx;
		size_t back_ind = 0;
		char data[chunk_size];
		//free listの導入で増えっぱなしなのでdequeが最速
		std::pmr::list<inst_info> inst_list;
		//append_only_deque inst_list;
		inst_info* free_head = nullptr;

		allocator(){

		}

		~allocator() {

		}

		template<typename t>
		bool allocateable() const noexcept{
		
			return allo_deallo_mtx.try_lock() &&
				//要素が確保できる場合にはfalseが帰るので反転
				!(
					//要素が確保できない場合にtrueを返すので後ろを実行できる
					!((back_ind + (uintptr_t)&data[back_ind] % alignof(t)) + sizeof(t) < chunk_size) &&
					//要素が確保できない場合には即座にアンロック
					(allo_deallo_mtx.unlock(), true));
		}

		template<typename t>
		t** construct() {
			inst_info* info_ptr;


			{
				std::lock_guard lock(inst_mtx);

				if (free_head) {
					info_ptr = free_head;			
					free_head = free_head->next_free;
				}
				else {
				 	info_ptr = &inst_list.emplace_back();
				}
			}
			info_ptr->next_free = reinterpret_cast<inst_info*>(0xfffffffff);
			info_ptr->move = &move<t>;


			{
				//次の取得アドレスがアラインに沿っているか
				back_ind += (uintptr_t)&data[back_ind] % alignof(t);
				info_ptr->ptr = &data[back_ind];
				back_ind += sizeof(t);
				allo_deallo_mtx.unlock();
			}


			if constexpr (!std::is_trivially_copyable_v<t>) {
				new (info_ptr->ptr) t();
			}
			return reinterpret_cast<t**>(reinterpret_cast<char*>(info_ptr));
		};


		template<typename t>
		t** back_ind_lock_construct() {
			inst_info* info_ptr;


			{
				std::lock_guard lock(inst_mtx);

				if (free_head) {
					info_ptr = free_head;
					free_head = free_head->next_free;
				}
				else {
					info_ptr = &inst_list.emplace_back();
				}
			}

			info_ptr->next_free = reinterpret_cast<inst_info*>(0xfffffffff);
			info_ptr->move = &move<t>;

			{
				std::lock_guard lock(allo_deallo_mtx);

				//次の取得アドレスがアラインに沿っているか
				back_ind += (uintptr_t)&data[back_ind] % alignof(t);
				info_ptr->ptr = &data[back_ind];
				back_ind += sizeof(t);
				
			}


			if constexpr (!std::is_trivially_copyable_v<t>) {
				new (info_ptr->ptr) t();
			}

			return reinterpret_cast<t**>(reinterpret_cast<char*>(info_ptr));
		};

		void gc() {

			decltype(inst_list.end()) end;
			decltype(inst_list.end()) i;

			std::lock_guard lock(allo_deallo_mtx);
			{
				std::lock_guard lock(inst_mtx);
				inst_list.sort();
				i = inst_list.begin();
				end = inst_list.end();
			}
			char* next = data;

			
			//無効なinfoもまとめてイテレートする為注意
			for(; i != end;++i)
			{
				auto p = &(*i);

				ptr_mtx->lock(p);
				//無効インスタンス避け
				if (p->ptr == nullptr) {
					ptr_mtx->unlock(p);
					continue;
				}

				{
					auto src = p->ptr;
					bool destroy_flag = p->next_free == nullptr;

					p->ptr = next;
					next = p->move(p->ptr, src, destroy_flag);//削除する場合next freeがnull

					std::lock_guard inst_lock(inst_mtx);
					
					if (destroy_flag) {
						p->next_free = free_head;
						free_head = p;
						p->ptr = nullptr;//インスタンス無効化
					}
				}

				ptr_mtx->unlock(p);
				
			}

			back_ind = (next - &data[0]);
		}

	};

	struct inst_deleter {
		void operator()(void* block)const {
			static_assert(std::is_standard_layout_v<inst_info>, "ポインタ変換が行えない環境");
			//デストラクトはGCで行う為宣言だけ行う
			ptr_mtx->lock(block);//gcスレッドとのアクセス競合の為削除してはいけない
			reinterpret_cast<inst_info*>(block)->next_free = nullptr;//削除フラグの代わり
			ptr_mtx->unlock(block);
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
	static char* move(char*& dst,char* src,bool src_destroy) {

		if (src_destroy) {
			if constexpr (!std::is_trivially_copyable_v<t>) {
				reinterpret_cast<t*>(src)->~t();
			}
			return dst;
		}
		

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

	std::list<std::unique_ptr<allocator>> chunk_arr;
	std::mutex chunk_mtx;

	static inline mt_mempool* inst = nullptr;
	static inline mempool_util::pointer_mutex* ptr_mtx;

	mt_mempool() {
		assert(inst == nullptr);
		inst = this;
		ptr_mtx = &mempool_util::get_pointer_mutex();
		
		if (chunk_arr.empty()) {
			chunk_arr.emplace_back(std::make_unique<allocator>());
		}
	}
	~mt_mempool() {
		if(inst == this)
			inst = nullptr;
	}

	template<typename t>
	std::unique_ptr<t*,inst_deleter> construct() {

		for (auto& chunk : chunk_arr)
		{
			if (chunk->allocateable<t>()) {

				return std::unique_ptr<t*, inst_deleter>(
					chunk->construct<t>()
				);
			}
		}
		allocator* owner;

		{
			std::lock_guard lock(chunk_mtx);
			owner = chunk_arr.emplace_back(std::make_unique<allocator>()).get();
		}

		return std::unique_ptr<t*, inst_deleter>(
			owner->back_ind_lock_construct<t>()
		);
	}

	void gc() {
		decltype(chunk_arr.begin()) itr;
		decltype(chunk_arr.begin()) end;

		{
			std::lock_guard lock(chunk_mtx);
			itr = chunk_arr.begin();
			end = chunk_arr.end();
		}
		
		for (; itr != end; ++itr)
		{
			(*itr)->gc();
		}
	}
};


template<typename t>
struct accessor {
	mempool_util::pointer_mutex& mtx;

	accessor() : mtx(mempool_util::get_pointer_mutex()) {}
	t& operator()(void* info) {
		return *(t*)((mt_mempool::inst_info*)info)->ptr;
	}

	void lock(const std::unique_ptr<t*,mt_mempool::inst_deleter>& ptr) {
		mtx.lock(ptr.get());
	}

	void unlock(const std::unique_ptr<t*, mt_mempool::inst_deleter>& ptr) {
		mtx.unlock(ptr.get());
	}
};




