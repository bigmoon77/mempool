#pragma once
#include <memory>
#include <shared_mutex>
#include <vector>
#include <typeindex>
#include <algorithm>
#include <memory_resource>
#include "error\located_exception.h"

/*
multi thread gcにて最大の課題とは

gcは常にメモリを監視し、空き領域を定期的に詰めていく
この時、アロケーション自体はロックフリーであることが望ましい？


*/


struct mt_mempool {
	
	struct type_info {
		std::type_index tid;
		size_t tsize  = 0;
		size_t talign = 0;
		
		void (*destruct)(void*, size_t) = nullptr;
		//src,dst
		void (*move)(char*, char*,size_t) = nullptr;


		bool operator < (const type_info& other)const {
			return tid < other.tid;
		}
		bool operator <= (const type_info& other)const {
			return tid <= other.tid;
		}
		bool operator == (const type_info& other)const {
			return tid == other.tid;
		}
	};

	struct type_map {
		std::vector<type_info> cont;
		std::shared_mutex cont_mtx;

		template<typename t>
		decltype(cont)::const_iterator add() {

			auto itr = std::lower_bound(//以上の最初の要素
				cont.begin(),
				cont.end(),
				type_info{ typeid(t) }
			);

			if ((itr != cont.end()) && itr->tid == typeid(t)) {
				return itr;
			}

			type_info new_info{ typeid(t) };

			new_info.tsize = sizeof(t);
			new_info.talign = sizeof(t);
			new_info.move = move<t>;
			new_info.destruct = destruct<t>;

			return cont.insert(itr, new_info);
		}

		decltype(cont)::const_iterator get(std::type_index tid) const {

			auto itr = std::lower_bound(//以上の最初の要素
				cont.begin(),
				cont.end(),
				type_info{ tid }
			);

			if ((itr != cont.end()) && itr->tid == tid) {
				return itr;
			}
		}

		auto cbegin() const {
			return cont.cbegin();
		}
		auto cend() const {
			return cont.cend();
		}


		std::shared_lock<std::shared_mutex> get_shared_lock() {
			return std::shared_lock(cont_mtx);
		}
		std::shared_lock<std::shared_mutex> get_lock() {
			return std::shared_lock(cont_mtx);
		}

		void sort() {
			//それなりに増えた時にsortオーバーヘッドが馬鹿にならない為
			auto temp = cont;
			std::sort(
				temp.begin(),
				temp.end());

			std::lock_guard lock(cont_mtx);
			cont.swap(temp);
		}

	};


	struct inst_info {
		void* ptr = nullptr;//オブジェクトが格納されたptrを指す
		size_t array_size = 1;
		std::type_index tid;
	};

	struct allocator {
		type_map tmap;
		std::pmr::unsynchronized_pool_resource& info_pool;

		std::unique_ptr<char[]> data;
		std::pmr::vector<inst_info*> info_vec;

		size_t back_ind = 0;
		size_t dead_space = 0;
		size_t max_ind = 0;

		allocator(std::pmr::unsynchronized_pool_resource& info_pool,size_t size)
			: info_pool(info_pool),
			data(std::make_unique_for_overwrite<char[]>(size)),
			max_ind(size) {
		}

		void* allocate(size_t size,size_t align) {
			
			//次の取得アドレスがアラインに沿っているか
			back_ind += (uintptr_t)&data[back_ind] % align;

			return &data[back_ind];
		}

		void deallocate(const void* block,size_t size, size_t align) {
			dead_space += size;
		}

		bool allocateable(size_t size,size_t align) const {
			return (back_ind + (uintptr_t)&data[back_ind] % align) <= max_ind;
		}


		inst_info* make_inst(void* ptr,size_t array_size,std::type_index tid) {

			auto info_ptr = (inst_info*)info_pool.allocate(sizeof(inst_info), alignof(inst_info));

			info_ptr->ptr = ptr;
			info_ptr->array_size = array_size;
			info_ptr->tid = tid;

			info_vec.emplace_back(info_ptr);
			return info_ptr;
		}

		void erase_inst(inst_info* ptr) {
			auto itr = std::find(info_vec.begin(), info_vec.end(), ptr);
			info_vec.erase(itr);
		}


		void gc() {

			auto head = data.get();

			for (auto& info_ptr : info_vec)
			{
				auto type = tmap.get(info_ptr->tid);
				
				type->move();

			}

			
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

			auto info = reinterpret_cast<inst_info*>(block);

			if constexpr (!std::is_trivially_copyable_v<t>) {//トリビアル型以外はデストラクト	
				for (size_t i = 0; i < info->array_size; i++)
				{
					reinterpret_cast<t*>(info->ptr)[i].~t();
				}
			}
			owner->deallocate(info->ptr, sizeof(t), alignof(t));
			owner->info_pool.deallocate(info, sizeof(inst_info), alignof(inst_info));
		}
	};


	template<typename t>
	static void destruct(void* block,size_t array_size) {
		for (size_t i = 0; i < array_size; i++)
		{
			reinterpret_cast<t*>(block)[i].~t();
		}
	}

	template<typename t>
	static void move(char* dst,char* src,size_t size) {

		if constexpr (std::is_trivially_copyable_v<t>) {
			std::memmove(dst, src, sizeof(t));
		}
		else {
#ifdef _DEBUG
			if (dst < src) {//dstの方が前であるべき
				throw error::located_exception("fetal error invalid object move");
			}
#endif

			//アドレス距離がオブジェクトのサイズ未満だった場合、移動先と移動元は重なっている

			if ((dst - src) < sizeof(t)) {//間が１要素以上あるならば問題はない為この境界

				for (size_t i = 0; i < size; i++)
				{
					t temp(std::move(
						*reinterpret_cast<t*>(src)
					));

					reinterpret_cast<t*>(src)[i].~t();

					new (&reinterpret_cast<t*>(dst)[i]) t(std::move(temp));
				}
			}
			else {

				for (size_t i = 0; i < size; i++)
				{
					new (&reinterpret_cast<t*>(dst)[i]) t(
						std::move(
							reinterpret_cast<t*>(src)[i]
						)
					);
					reinterpret_cast<t*>(src)[i].~t();
				}
			}

		}
		
	}



	std::vector<allocator> chunk_arr;

	std::vector<inst_info*> info_arr;

	std::pmr::unsynchronized_pool_resource info_pool;

	type_map type_map;

	size_t last_size = 256;

	
	using t = int;
	std::unique_ptr<t*,inst_deleter<t>> construct(size_t size = 1) {

		if (chunk_arr.empty()) {
			last_size *= 2;
			chunk_arr.emplace_back(info_pool, last_size);
		}
		
		t* ptr = nullptr;
		allocator* owner = nullptr;
		for (auto& chunk: chunk_arr)
		{
			if (chunk.allocateable(sizeof(t), alignof(t))) {
				ptr = reinterpret_cast<t*>(chunk.allocate(sizeof(t) * size, alignof(t)));
				owner = &chunk;
				break;
			}
		}

		//construct
		for (size_t i = 0; i < size; i++)
		{
			new (&ptr[i])t();
		}

		auto info_ptr = reinterpret_cast<inst_info*>(info_pool.allocate(sizeof(inst_info) * size, alignof(inst_info)));

		info_ptr->ptr = ptr;
		info_ptr->tid = typeid(t);
		info_ptr->array_size = size;

		info_arr.emplace_back(info_ptr);

		return std::unique_ptr<t*, inst_deleter<t>>((t**)(char*)info_ptr, inst_deleter<t>(owner));
	}




};