#pragma once
#include <memory>
#include <shared_mutex>
#include <vector>
#include <typeindex>
#include <algorithm>
#include <memory_resource>


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
		std::type_index tid;
		size_t array_size = 1;
		void* ptr = nullptr;//オブジェクトが格納されたptrを指す
	};

	struct inst_deleter {
		allocator* owner = nullptr;
		inst_deleter(allocator* owner) : owner(owner) {};
		inst_deleter(const inst_deleter& other) = default;
		inst_deleter(inst_deleter&& other) = default;

		using t = int;

		void operator()(const void* block)const {
			if constexpr (!std::is_trivially_copyable_v<t>) {//トリビアル型以外はデストラクト
				reinterpret_cast<const t*>(block)->~t();
			}
			owner->deallocate(block, sizeof(t), alignof(t));
		}
	};

	class accessor {
		inst_info* info = nullptr;
		accessor(inst_info& info) : info(&info) {

		}
		using t = int;

		t* get() {
			return reinterpret_cast<t*> (info->ptr);
		}
		const t* get() const {
			return reinterpret_cast<const t*> (info->ptr);
		}

		size_t size()const {
			return info->array_size;
		}

		std::type_index get_type() const {
			return info->tid;
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
	static void move(void* src, void* dst) {

		


	}
	static void move(void* src, void* dst,size_t src_size,size_t dst_size) {

		
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