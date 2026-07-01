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

		static thread_local inline uint8_t current_tid = 1;
		static inline std::atomic_uint8_t counter = 0;

		struct entry  {
			std::atomic_uint8_t mtx = 0;

			void lock() {

				while (true)
				{
					uint8_t temp = 0;
					if (mtx.compare_exchange_weak(temp, current_tid))
						break;
					mtx.wait(temp);
				}

			}

			void unlock() {
				mtx.store(0);
				mtx.notify_one();
			}
		};


		static inline constexpr size_t size = mempool_util::pow2<10>();
		static inline constexpr size_t mask = size - 1;

		entry arr[size];

		static inline size_t make_hash(void* ptr)noexcept {
			return (uintptr_t)ptr << 4;
		}

		static void init_thread() {
			current_tid = ++counter;
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

	struct type_info {
		std::type_index tid = typeid(void);
		//dst,src
		char* (*move)(char*&, char*) = nullptr;

		bool operator < (const type_info& other)const noexcept {
			return tid < other.tid;
		}
		bool operator > (const type_info& other)const noexcept {
			return tid < other.tid;
		}
		bool operator <= (const type_info& other)const noexcept {
			return tid <= other.tid;
		}
		bool operator >= (const type_info& other)const noexcept {
			return tid >= other.tid;
		}
		bool operator == (const type_info& other)const noexcept {
			return tid == other.tid;
		}

		bool operator != (const type_info& other)const noexcept {
			return tid != other.tid;
		}
	};

	struct inst_info {
		char* ptr = nullptr;//オブジェクトが格納されたptrを指す
		std::type_index tid = typeid(void);
	};

	struct allocator {
		mempool_util::pointer_mutex* ptr_mtx;

		mutable std::mutex allo_deallo_mtx;
		mutable std::mutex tmap_mtx;
		mutable std::mutex wait_inst_mtx;

		std::set<type_info> tmap;


		std::vector<size_t> wait_inst_index_arr;
		//gc中に追加されたオブジェクト
		std::vector<size_t> gc_make_inst;
		//次オブジェクトを作成する際のindex
		size_t back_ind = 0;

		//次作成する際のinst_infoのindex
		size_t inst_ind = 0;

		bool gc_now = false;

		char data[chunk_size];

		allocator() {
			ptr_mtx = &mempool_util::get_pointer_mutex();
			gc_make_inst.reserve(chunk_size / 16);
			wait_inst_index_arr.reserve(chunk_size / 16);
		}

		~allocator() {

#ifdef _DEBUG

			auto tail = reinterpret_cast<inst_info*>(&data[chunk_size - sizeof(inst_info)]);
			size_t j = 0;
			for (size_t i = 0; i < inst_ind; i++)
			{
				if (i == wait_inst_index_arr[j]) {
					++j;
					continue;
				}

				std::cerr << "leak " << (tail - i)->ptr << std::endl;
			}
#endif
		}

		bool allocateable(size_t size, size_t align) const noexcept{
			std::lock_guard lock(allo_deallo_mtx);
			return (back_ind + ((uintptr_t)&data[back_ind] % align)) + size < (chunk_size - (sizeof(inst_info) *
				(inst_ind + 1)//これから追加するインスタンス情報分
				));
		}

		template<typename t>
		t** construct() {

			inst_info* info_ptr;

			{
				std::lock_guard lock(wait_inst_mtx);

				if (wait_inst_index_arr.empty()) {
					info_ptr =
						reinterpret_cast<inst_info*>(&data[chunk_size - sizeof(inst_info)]) - (inst_ind++);
				}
				else {
					info_ptr =
						reinterpret_cast<inst_info*>(&data[chunk_size - sizeof(inst_info)]) -
						wait_inst_index_arr.back();
					wait_inst_index_arr.pop_back();
				}
				if (gc_now) {
					gc_make_inst.emplace_back(//gc中に生成された物ならばindexを記録
						static_cast<size_t>(
							(inst_info*)&data[chunk_size - sizeof(inst_info)] - info_ptr
							)
					);
				}
			}



			{
				std::lock_guard lock(tmap_mtx);
				tmap.emplace(typeid(t), &move<t>);
			}

			



			{
				std::lock_guard lock(allo_deallo_mtx);
				//次の取得アドレスがアラインに沿っているか
				back_ind += (uintptr_t)&data[back_ind] % alignof(t);


				ptr_mtx->lock(info_ptr);

				info_ptr->ptr = &data[back_ind];//info更新　この時点でデストラクタは呼ばれている必要がある
				back_ind += sizeof(t);
			}

			if constexpr (!std::is_trivially_copyable_v<inst_info>) {
				new (info_ptr) inst_info();
			}

			info_ptr->tid = typeid(t);

			if constexpr (!std::is_trivially_copyable_v<t>) {
				new (info_ptr->ptr) t();
			}


			ptr_mtx->unlock(info_ptr);

			return (t**)(char*)info_ptr;
		};


		//オブジェクトへのデストラクは呼ばない
		void erase_inst(inst_info* ptr) {

			//gcが終わるまで待機

			{
				ptr_mtx->lock(ptr);

				if constexpr (!std::is_trivially_copyable_v<inst_info>) {
					ptr->~inst_info();
				}

				ptr->tid = typeid(void);
				ptr_mtx->unlock(ptr);
			}

			auto wait_ind = 
				static_cast<size_t>(
					reinterpret_cast<inst_info*>(&data[chunk_size - sizeof(inst_info)]) -//最後尾のptr
					ptr
				);

			std::lock_guard lock(wait_inst_mtx);
			
			wait_inst_index_arr.insert(
				std::lower_bound(
					wait_inst_index_arr.begin(), wait_inst_index_arr.end(),
					wait_ind
				),//insert_sort
				wait_ind
			);
		}


		void gc() {

			auto next_ptr = data;

			size_t wait_ind = 0;//wait_ind[wait_ind]の部分は飛ばして読む
			

			//ダブルバッファリング
			std::vector<size_t> temp_wait_inst_index_arr;
			std::set<type_info> tmap;

			{
				std::lock_guard tmap_lock(tmap_mtx);
				tmap = this->tmap;
			}
			
			size_t temp_inst_ind;
			
			
			{
				std::lock_guard lock(wait_inst_mtx);
				temp_wait_inst_index_arr = wait_inst_index_arr;
				temp_inst_ind = inst_ind;

				gc_now = true;//追加もここのmtxで行われるので漏らしはない
			}


			//非同期フェーズ
#pragma region
			{
				std::vector<size_t> inst_ind_arr;
				inst_ind_arr.resize(temp_inst_ind - temp_wait_inst_index_arr.size());


				if (temp_wait_inst_index_arr.size()) {
					size_t j = 0;
					size_t k = 0;

					for (size_t i = 0; i < temp_inst_ind; i++)
					{
						if (i != temp_wait_inst_index_arr[j]) {//iは待機indexではない
							inst_ind_arr[k++] = i;//そのindexが待機indexかを見る
						}
						else {


							if (temp_wait_inst_index_arr.size() == ++j) {//あとは空白無し

								for (size_t l = i + 1; l < temp_inst_ind; ++l)
								{
									inst_ind_arr[k++] = l;
								}

								break;
							}
						}

					}
				}
				else {

					for (size_t i = 0; i < temp_inst_ind; i++)
					{
						inst_ind_arr[i] = i;
					}
				}

				//最後尾のinst_info
				//ケツから確保しているので注意
				auto inst_tail = reinterpret_cast<inst_info*>(&data[chunk_size - sizeof(inst_info)]);

				std::sort(inst_ind_arr.begin(), inst_ind_arr.end(),
					[&inst_tail](const size_t& l, const size_t& r) {
						return
							(inst_tail - l)->ptr <
							(inst_tail - r)->ptr;
					});


				for (auto& i : inst_ind_arr)
				{
					inst_info* inst_ptr = (inst_tail - i);

					ptr_mtx->lock(inst_ptr);
					if (inst_ptr->tid == typeid(void)) {//途中で破棄された
						ptr_mtx->unlock(inst_ptr);
						continue;
					}

					auto preview_loc = inst_ptr->ptr;
					inst_ptr->ptr = next_ptr;//new loc

					next_ptr = tmap.find({ inst_ptr->tid,nullptr })->move(inst_ptr->ptr, preview_loc);

					ptr_mtx->unlock(inst_ptr);

				}
			}


#pragma endregion


			//同期フェーズ
#pragma region

			{
				//最後尾のinst_info
				//ケツから確保しているので注意
				auto inst_tail = reinterpret_cast<inst_info*>(&data[chunk_size - sizeof(inst_info)]);

				//backindの固定に必要
				std::lock_guard allo_lock(allo_deallo_mtx);
				//gc中に追加されたオブジェクトだけ追跡する機能が必要
				//back_indexの整合性を保つには、gc中追加されたインスタンスを走査し、これらのメモリ位置を変更する必要がある
				
				for (auto& i : gc_make_inst)
				{
					inst_info* inst_ptr = (inst_tail - i);



					ptr_mtx->lock(inst_ptr); 
					if (inst_ptr->tid == typeid(void)) {//途中で破棄された
						ptr_mtx->unlock(inst_ptr);
						continue;
					}

					auto type = tmap.find({ inst_ptr->tid,nullptr });

					auto preview_loc = inst_ptr->ptr;
					inst_ptr->ptr = next_ptr;//new loc

					//アラインメント考慮は関数が行う
					//帰り値は次のptr
					next_ptr = type->move(inst_ptr->ptr, preview_loc);

					ptr_mtx->unlock(inst_ptr);

				}

				back_ind = static_cast<size_t>(next_ptr - data);

				std::lock_guard wait_lock(wait_inst_mtx);
				gc_now = false;
			}
#pragma endregion



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
				reinterpret_cast<t*>(reinterpret_cast<inst_info*>(block)->ptr)->~t();
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

		for (auto& chunk : chunk_arr)
		{
			if (chunk->allocateable(sizeof(t), alignof(t))) {

				return std::unique_ptr<t*, inst_deleter<t>>(
					chunk->construct<t>(),
					inst_deleter<t>(chunk.get())
				);
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
		//mempool_util::get_pointer_mutex().lock(ptr.get());
		mtx.lock(ptr.get());
	}

	void unlock(const std::unique_ptr<t*, mt_mempool::inst_deleter<t>>& ptr) {
		mtx.unlock(ptr.get());
	}
};




