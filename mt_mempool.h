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


struct mempool {
	
	struct type_info {
		std::type_index tid;
		size_t tsize  = 0;
		size_t talign = 0;
		
		void (*destruct)(void*)	   = nullptr;
		//src,dst
		void (*move)(void*, void*) = nullptr;


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

	struct allocator {
		std::unique_ptr<char[]> data;
		size_t back_ind = 0;
		size_t dead_space = 0;

		allocator(size_t size)
			: data(std::make_unique_for_overwrite<char[]>(size)) {		
		}

		void* allocate(size_t size,size_t align) {
			
			//次の取得アドレスがアラインに沿っているか
			auto remind = (uintptr_t)&data[back_ind] % align;
			if (remind) {//あまりがある場合、その分の余剰を取る
				back_ind += remind;
			}

			return &data[back_ind];
		}

		void deallocate(const void* block,size_t size, size_t align) {
			dead_space += size;
		}

	};

	struct inst_info {
		void* ptr = nullptr;//オブジェクトが格納されたptrを指す
		size_t array_size = 1;
		std::type_index tid;
	};

	struct inst_deleter {
		allocator* owner = nullptr;
		inst_deleter(allocator* owner) : owner(owner) {};
		inst_deleter(const inst_deleter& other) = default;
		inst_deleter(inst_deleter&& other) = default;

		using t = int;

		void operator()(const void* block)const {
			auto info = reinterpret_cast<const inst_info*>(block);

			if constexpr (!std::is_trivially_copyable_v<t>) {//トリビアル型以外はデストラクト	
				for (size_t i = 0; i < info->array_size; i++)
				{
					reinterpret_cast<t*>(info->ptr)[i].~t();
				}
			}
			owner->deallocate(info->ptr, sizeof(t), alignof(t));
		}
	};

	std::vector<allocator> chunk_arr;


	//template<typename t>
	static void destruct(void* block,size_t array_size) {
		for (size_t i = 0; i < array_size; i++)
		{
			reinterpret_cast<t*>(block)[i].~t();
		}
	}


	//template<typename t>
	using t = int;

	//single,single
	static void move(char* dst, char* src) {

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

			if ((dst - src) < sizeof(t)) {

				t temp (std::move(
					*reinterpret_cast<t*>(src)
				));

				reinterpret_cast<t*>(src)->~t();

				new (dst)t(std::move(temp));
			}
			else {
				new (dst) t(
					std::move(
						*reinterpret_cast<t*>(src)
					)
				);
				reinterpret_cast<t*>(src)->~t();
			}

		}

	}
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

				t temp(std::move(
					*reinterpret_cast<t*>(src)
				));

				reinterpret_cast<t*>(src)->~t();

				new (dst)t(std::move(temp));
			}
			else {
				new (dst) t(
					std::move(
						*reinterpret_cast<t*>(src)
					)
				);
				reinterpret_cast<t*>(src)->~t();
			}

		}
		
	}



	struct type_map {
		std::vector<type_info> cont;
		std::shared_mutex cont_mtx;
		using t = int;
		
		//template<typename t>
		void add() {
			//それなりに増えた時にsortオーバーヘッドが馬鹿にならない為
			auto temp = cont;

			std::sort(
				temp.begin(),
				temp.end());

			std::lock_guard lock(cont_mtx);
			cont.swap(temp);
		}

		decltype(cont)::const_iterator get() {
			std::shared_lock lock(cont_mtx);

			auto itr = std::lower_bound(//以上の最初の要素
				cont.begin(),
				cont.end(),
				type_info{ typeid(t) }
			);
			
			if ((itr != cont.end()) && itr->tid == typeid(t)) {
				return itr;
			}

			type_info new_info{typeid(t)};

			new_info.tsize = sizeof(t);
			new_info.talign = sizeof(t);

			cont.insert(itr,);

		}

		type_info& get() {





		}
	};
		 

	




};