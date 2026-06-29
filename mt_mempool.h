#pragma once
#include <memory>
#include <shared_mutex>
#include <vector>
#include <atomic>
#include <set>
#include <typeindex>
#include <algorithm>
#include <memory_resource>
#include "error/located_exception.h"

/*
multi thread gcにて最大の課題とは

gcは常にメモリを監視し、空き領域を定期的に詰めていく
この時、アロケーション自体はロックフリーであることが望ましい？


*/


struct mt_mempool {
	
	struct type_info {
		std::type_index tid = typeid(int);

		void (*destruct)(void*, size_t) = nullptr;
		//dst,src
		char* (*move)(char*&, char*,size_t) = nullptr;


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
	/*
	struct type_map {
		std::vector<type_info> cont;
		std::shared_mutex cont_mtx;

		type_map() = default;


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
	*/
	struct inst_info {
		char* ptr = nullptr;//オブジェクトが格納されたptrを指す
		size_t array_size = 1;
		std::type_index tid = typeid(int);
	};

	struct allocator {
		//type_map tmap;

		mutable std::mutex allo_deallo_mtx;
		mutable std::mutex tmap_mtx;
		mutable std::mutex wait_inst_mtx;
		mutable std::mutex ptr_mtx;

		std::set<type_info> tmap;

		std::unique_ptr<char[]> data;

		//次オブジェクトを作成する際のindex
		size_t back_ind = 0;

		//次作成する際のinst_infoのindex
		size_t inst_ind = 0;
		
	
		//取りえる最大のindex
		size_t max_ind = 0;

		//破棄したオブジェクトの総サイズ
		std::atomic_size_t dead_space = 0;
		/// <summary>
		/// ロックされているinst_infoのptr
		/// </summary>
		std::atomic<char*> locked_ptr = nullptr;

		std::vector<size_t> wait_inst_index_arr;

		//gc中に追加されたオブジェクト
		std::vector<size_t> gc_make_inst;
		bool gc_now = false;


		allocator(size_t size)
			: data(std::make_unique_for_overwrite<char[]>(size)),
			max_ind(size) {
		}

		~allocator() {

#ifdef _DEBUG
			
			auto tail = reinterpret_cast<inst_info*>(&data[max_ind - sizeof(inst_info)]);
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

		char* allocate(size_t size,size_t align) {
			std::lock_guard lock(allo_deallo_mtx);

			//次の取得アドレスがアラインに沿っているか
			back_ind += (uintptr_t)&data[back_ind] % align;
			auto res = &data[back_ind];

			back_ind += size;

			return res;
		}

		void deallocate(const char* block,size_t size, size_t align) {
			dead_space += size;
		}

		bool allocateable(size_t size,size_t align) const {
			std::lock_guard lock(allo_deallo_mtx);
			return (back_ind + ((uintptr_t)&data[back_ind] % align)) +
				(sizeof(inst_info) * inst_ind) <= max_ind;
		}


		//オブジェクトへのコンストラクタは呼ばない
		template<typename t>
		inst_info* make_inst(char* ptr,size_t array_size,std::type_index tid) {
			
			inst_info* info_ptr;

			{
				std::lock_guard lock(wait_inst_mtx);

				if (wait_inst_index_arr.empty())
					info_ptr = reinterpret_cast<inst_info*>(&data[max_ind - sizeof(inst_info)]) -
					(inst_ind++);
				else {
					info_ptr = reinterpret_cast<inst_info*>(&data[max_ind - sizeof(inst_info)]) -
						wait_inst_index_arr.back();
					wait_inst_index_arr.pop_back();
				}


				if (gc_now) {

					gc_make_inst.emplace_back(//gc中に生成された物ならばindexを記録
						static_cast<size_t>(
							(
								inst_info*)&data[max_ind - sizeof(inst_info)] - info_ptr
							)
					);
				}

			}



			{//型追加
				type_info temp;
				temp.tid = tid;
				temp.destruct = &destruct<t>;
				temp.move = &move<t>;

				std::lock_guard lock(tmap_mtx);
				tmap.emplace(temp);
			}


			{
				
				std::lock_guard lock(ptr_mtx);

				if constexpr (!std::is_trivially_copyable_v<inst_info>) {
					new (info_ptr) inst_info();
				}

				locked_ptr.store((char*) info_ptr,std::memory_order_release);

				info_ptr->ptr = ptr;//info更新　この時点でデストラクタは呼ばれている必要がある
				info_ptr->array_size = array_size;
				info_ptr->tid = tid;

				locked_ptr.store(nullptr, std::memory_order_release);

			}

			return info_ptr;
		}


		//オブジェクトへのデストラクは呼ばない
		void erase_inst(inst_info* ptr) {

			//gcが終わるまで待機

			{
				std::lock_guard lock(ptr_mtx);

				if constexpr (!std::is_trivially_copyable_v<inst_info>) {
					ptr->~inst_info();
				}

				ptr->array_size = static_cast<size_t>(-1);//破棄フラグ
			}

			auto wait_ind = 
				static_cast<size_t>(
					reinterpret_cast<inst_info*>(&data[max_ind - sizeof(inst_info)]) -//最後尾のptr
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

			auto next_ptr = data.get();

			size_t wait_ind = 0;//wait_ind[wait_ind]の部分は飛ばして読む
			
			//最後尾のinst_info
			//ケツから確保しているので注意
			auto inst_tail = reinterpret_cast<inst_info*>(&data[max_ind - sizeof(inst_info)]);


			//ダブルバッファリング
			std::vector<size_t> temp_wait_inst_index_arr;
			size_t temp_inst_ind;
			{
				std::lock_guard lock(wait_inst_mtx);
				temp_wait_inst_index_arr = wait_inst_index_arr;
				temp_inst_ind = inst_ind;

				gc_now = true;//追加もここのmtxで行われるので漏らしはない
			}
			

			//非同期フェーズ
#pragma region
			for (size_t i = 0; i < temp_inst_ind; i++)
			{
				if (i != temp_wait_inst_index_arr[wait_ind]) {

					inst_info* inst_ptr = (inst_tail - i);

					//破棄領域に再度オブジェクトが追加されていた場合書込みで競合する可能性がある為ロック
					std::lock_guard ptr_lock(ptr_mtx);

					locked_ptr.store((char*)inst_ptr, std::memory_order_release);
					if (inst_ptr->array_size == static_cast<size_t>(-1)) {//破棄済みフラグ
						locked_ptr.store(nullptr, std::memory_order_release);
						continue;
					}

					const type_info* type;

					{
						std::lock_guard tmap_lock(tmap_mtx);
						type = &(*tmap.find({ inst_ptr->tid,nullptr,nullptr }));
					}



					auto preview_loc = inst_ptr->ptr;
					inst_ptr->ptr = next_ptr;//new loc

					//アラインメント考慮は関数が行う
					//帰り値は次のptr
					next_ptr = type->move(inst_ptr->ptr, preview_loc, inst_ptr->array_size);

					locked_ptr.store(nullptr, std::memory_order_release);

				}
				else {
					if (temp_wait_inst_index_arr.size() == ++wait_ind) {
						//以降はwaitがないことが確定しているのでif無しloopに飛ばす

						for (size_t j = i + 1; j < temp_inst_ind; j++)
						{
							inst_info* inst_ptr = (inst_tail - j);


							std::lock_guard ptr_lock(ptr_mtx);

							locked_ptr.store((char*)inst_ptr, std::memory_order_release);
							if (inst_ptr->array_size == static_cast<size_t>(-1)) {
								locked_ptr.store(nullptr, std::memory_order_release);

								continue;
							}

							const type_info* type;

							{
								std::lock_guard tmap_lock(tmap_mtx);
								type = &(*tmap.find({ inst_ptr->tid,nullptr,nullptr }));
							}

							auto preview_loc = inst_ptr->ptr;
							inst_ptr->ptr = next_ptr;//new loc

							//アラインメント考慮は関数が行う
							//帰り値は次のptr
							next_ptr = type->move(inst_ptr->ptr, preview_loc, inst_ptr->array_size);
						}


						std::lock_guard ptr_lock(ptr_mtx);

						locked_ptr.store(nullptr, std::memory_order_release);

						break;
					}
					continue;
				}
			}

#pragma endregion


			//同期フェーズ
#pragma region

			{
				//backindの固定に必要
				std::lock_guard allo_lock(allo_deallo_mtx);
				//gc中に追加されたオブジェクトだけ追跡する機能が必要
				//back_indexの整合性を保つには、gc中追加されたインスタンスを走査し、これらのメモリ位置を変更する必要がある
				
				for (auto& i : gc_make_inst)
				{
					inst_info* inst_ptr = (inst_tail - i);
					std::lock_guard ptr_lock(ptr_mtx);

					locked_ptr.store(inst_ptr->ptr, std::memory_order_release);
					if (inst_ptr->array_size == static_cast<size_t>(-1)) {//途中で破棄された
						locked_ptr.store(nullptr, std::memory_order_release);
						continue;
					}

					auto type = tmap.find({ inst_ptr->tid,nullptr,nullptr });

					auto preview_loc = inst_ptr->ptr;
					inst_ptr->ptr = next_ptr;//new loc

					//アラインメント考慮は関数が行う
					//帰り値は次のptr
					next_ptr = type->move(inst_ptr->ptr, preview_loc, inst_ptr->array_size);
				}

				std::lock_guard ptr_lock(ptr_mtx);

				locked_ptr.store(nullptr, std::memory_order_release);
				back_ind = static_cast<size_t>(next_ptr - data.get());

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

			auto info = reinterpret_cast<inst_info*>(block);

			if constexpr (!std::is_trivially_copyable_v<t>) {//トリビアル型以外はデストラクト	
				for (size_t i = 0; i < info->array_size; i++)
				{
					reinterpret_cast<t*>(info->ptr)[i].~t();
				}
			}
			owner->deallocate(info->ptr, sizeof(t), alignof(t));
			owner->erase_inst(info);
		}
	};


	template<typename t>
	static void destruct(void* block,size_t array_size) {
		for (size_t i = 0; i < array_size; i++)
		{
			reinterpret_cast<t*>(block)[i].~t();
		}
	}

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
	static char* move(char*& dst,char* src,size_t size) {

		dst += ((uintptr_t)dst % alignof(t));//アラインメント詰めのアドレスにする

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


		return dst + (sizeof(t) * size);
	}



	std::vector<std::unique_ptr<allocator>> chunk_arr;

	size_t last_size = 256;

	
	using t = int;
	std::unique_ptr<t*,inst_deleter<t>> construct(size_t size = 1) {

		if (chunk_arr.empty()) {
			last_size *= 2;
			chunk_arr.emplace_back(std::make_unique<allocator>(last_size));
		}
		
		t* ptr = nullptr;
		allocator* owner = nullptr;
		for (auto& chunk: chunk_arr)
		{
			if (chunk->allocateable(sizeof(t), alignof(t))) {
				ptr = reinterpret_cast<t*>(chunk->allocate(sizeof(t) * size, alignof(t)));
				owner = chunk.get();
				break;
			}
		}

		//construct
		for (size_t i = 0; i < size; i++)
		{
			new (&ptr[i])t();
		}

		auto info_ptr = owner->make_inst<t>((char*)ptr, size, typeid(t));

		return std::unique_ptr<t*, inst_deleter<t>>((t**)(char*)info_ptr, inst_deleter<t>(owner));
	}




};