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


#include "error/located_exception.h"


struct mt_mempool {
	struct pointer_barrier {
		struct pointer_info {
			void* loc;
			std::shared_mutex mtx;
		};

		std::map<std::thread::id, pointer_info> vals;
		
		void init_thread(std::thread::id id) {
			vals[id].loc = nullptr;
		}

		void lock(void* ptr) {
			auto this_id = std::this_thread::get_id();
			bool loop;

			do
			{
				loop = false;


				for (auto& [k, v] : vals)
				{
					if (k == this_id)
						continue;

					v.mtx.lock_shared();
					loop = loop || v.loc == ptr;
				}

				//全ての値にロックを取得しており変更される可能性がない
				//かつ　全ての値がロック対象の値と違う場合、ロックを実行する

				if (!loop) {
					auto& target = vals[this_id];
					std::lock_guard lock(target.mtx);
					target.loc = ptr;
				}

				for (auto& [k,v] : vals)
				{
					if (k == this_id)
						continue;
					v.mtx.unlock_shared();
				}

			} while (loop);

		}
		void unlock() {
			auto this_id = std::this_thread::get_id();
			
			auto& target = vals[this_id];
			std::lock_guard lock(target.mtx);
			target.loc = nullptr;
		}
	};

	struct type_info {
		std::type_index tid = typeid(int);
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

	struct inst_info {
		char* ptr = nullptr;//オブジェクトが格納されたptrを指す
		size_t array_size = 1;
		std::type_index tid = typeid(int);
	};

	struct allocator {
		//type_map tmap;
		pointer_barrier& ptr_barrier;

		mutable std::mutex allo_deallo_mtx;
		mutable std::mutex tmap_mtx;
		mutable std::mutex wait_inst_mtx;

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
		

		std::vector<size_t> wait_inst_index_arr;

		//gc中に追加されたオブジェクト
		std::vector<size_t> gc_make_inst;
		
		bool gc_now = false;


		allocator(pointer_barrier& ptr_barrier,size_t size)
			: ptr_barrier(ptr_barrier),
			data(std::make_unique_for_overwrite<char[]>(size)),
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
				temp.move = &move<t>;

				std::lock_guard lock(tmap_mtx);
				tmap.emplace(temp);
			}


			{
				ptr_barrier.lock(info_ptr);

				if constexpr (!std::is_trivially_copyable_v<inst_info>) {
					new (info_ptr) inst_info();
				}

				info_ptr->ptr = ptr;//info更新　この時点でデストラクタは呼ばれている必要がある
				info_ptr->array_size = array_size;
				info_ptr->tid = tid;
				
				ptr_barrier.unlock();
			}

			return info_ptr;
		}


		//オブジェクトへのデストラクは呼ばない
		void erase_inst(inst_info* ptr) {

			//gcが終わるまで待機

			{
				ptr_barrier.lock(ptr);

				if constexpr (!std::is_trivially_copyable_v<inst_info>) {
					ptr->~inst_info();
				}

				ptr->array_size = static_cast<size_t>(-1);//破棄フラグ

				ptr_barrier.unlock();
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

					ptr_barrier.lock(inst_ptr);
					if (inst_ptr->array_size == static_cast<size_t>(-1)) {//途中で破棄された
						continue;
					}
					ptr_barrier.unlock();



					const type_info* type;

					{
						std::lock_guard tmap_lock(tmap_mtx);
						type = &(*tmap.find({ inst_ptr->tid,nullptr }));
					}



					auto preview_loc = inst_ptr->ptr;
					inst_ptr->ptr = next_ptr;//new loc

					//アラインメント考慮は関数が行う
					//帰り値は次のptr
					next_ptr = type->move(inst_ptr->ptr, preview_loc, inst_ptr->array_size);
				}
				else {
					if (temp_wait_inst_index_arr.size() == ++wait_ind) {
						//以降はwaitがないことが確定しているのでif無しloopに飛ばす

						for (size_t j = i + 1; j < temp_inst_ind; j++)
						{
							inst_info* inst_ptr = (inst_tail - j);


							ptr_barrier.lock((char*)inst_ptr);
							if (inst_ptr->array_size == static_cast<size_t>(-1)) {//途中で破棄された
								continue;
							}
							ptr_barrier.unlock();


							const type_info* type;

							{
								std::lock_guard tmap_lock(tmap_mtx);
								type = &(*tmap.find({ inst_ptr->tid,nullptr }));
							}

							auto preview_loc = inst_ptr->ptr;
							inst_ptr->ptr = next_ptr;//new loc

							//アラインメント考慮は関数が行う
							//帰り値は次のptr
							next_ptr = type->move(inst_ptr->ptr, preview_loc, inst_ptr->array_size);
						}


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


					ptr_barrier.lock(inst_ptr);
					if (inst_ptr->array_size == static_cast<size_t>(-1)) {//途中で破棄された
						continue;
					}
					ptr_barrier.unlock();


					auto type = tmap.find({ inst_ptr->tid,nullptr });

					auto preview_loc = inst_ptr->ptr;
					inst_ptr->ptr = next_ptr;//new loc

					//アラインメント考慮は関数が行う
					//帰り値は次のptr
					next_ptr = type->move(inst_ptr->ptr, preview_loc, inst_ptr->array_size);
				}

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

	pointer_barrier ptr_barrier;
	std::vector<std::unique_ptr<allocator>> chunk_arr;

	size_t last_size = 256;
	
	using t = int;
	std::unique_ptr<t*,inst_deleter<t>> construct(size_t size = 1) {

		if (chunk_arr.empty()) {
			last_size *= 2;
			chunk_arr.emplace_back(std::make_unique<allocator>(ptr_barrier,last_size));
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


	void init_thread() {
		ptr_barrier.init_thread(std::this_thread::get_id());
	}



};

struct accessor {
	using t = int;
	//template<typename t>
	t& operator()(t* info) {
		*(t*)((mt_mempool::inst_info*)(char*)info)->ptr;
	}

	void lock(std::unique_ptr<t,mt_mempool::inst_deleter<t>>& ptr) {
		ptr.get_deleter().owner->ptr_barrier.lock(ptr.get());
	}

	void unlock(std::unique_ptr<t, mt_mempool::inst_deleter<t>>& ptr) {
		ptr.get_deleter().owner->ptr_barrier.unlock();
	}
};