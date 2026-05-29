#pragma once

#include <located_exception.h>
#include <memory_resource>

/// <summary>
/// gc無しメモリプール
/// </summary>
class mempool {

	std::pmr::unsynchronized_pool_resource pool;

	template<typename t>
	static void deallocate(mempool* i, void* block) {
		i->pool.deallocate(block, sizeof(t));
	}

	template<typename t>
	static void* allocate(mempool* i) {
		return i->pool.allocate(sizeof(t));
	}

	template<typename t>
	static void* allocate(mempool* i, size_t size) {
		return i->pool.allocate(sizeof(t) * size);
	}

	template<typename t,size_t size>
	static void* allocate(mempool* i) {
		return i->pool.allocate(sizeof(t) * size);
	}


public:

	template<typename t>
	struct deleter {
		std::pmr::unsynchronized_pool_resource* pool = nullptr;
		deleter() = default;
		deleter(const deleter&) = default;
		deleter(std::pmr::unsynchronized_pool_resource& p) :pool(&p) {};
		void operator ()(void* block)const {
			((t*)block)->~t();
			pool->deallocate(block, sizeof(t));
		}
	};

	template<typename t>
	struct arr_deleter {
		std::pmr::unsynchronized_pool_resource* pool = nullptr;
		size_t size = 0;
		arr_deleter() = default;
		arr_deleter(const arr_deleter&) = default;
		arr_deleter(std::pmr::unsynchronized_pool_resource& p,size_t size) :pool(&p) ,size(size){};

		void operator ()(void* block)const {
			for (size_t i = 0; i < size; i++)
			{
				((t*)block)[i].~t();
			}
			pool->deallocate(block, sizeof(t) * size);
		}
	};

	template<typename t,size_t size>
	struct sarr_deleter {
		std::pmr::unsynchronized_pool_resource* pool = nullptr;
		sarr_deleter() = default;
		sarr_deleter(const sarr_deleter&) = default;
		sarr_deleter(std::pmr::unsynchronized_pool_resource& p) :pool(&p) {};

		void operator ()(void* block)const {

			for (size_t i = 0; i < size; i++)
			{
				((t*)block)[i].~t();
			}

			pool->deallocate(block, sizeof(t) * size);
		}
	};

	template<typename t>
	std::shared_ptr<t> construct_share() {
		return std::shared_ptr<t>(
			new (allocate<t>(this)) t(),
			deleter<t>(pool)
			);
	}

	template<typename t>
	std::shared_ptr<t[]> construct_share(size_t size) {
		auto head = allocate<t>(this,size);

		for (size_t i = 0; i < size; i++)
		{
			new (&((t*)head)[i]) t();
		}

		return std::shared_ptr<t[]>(
			head,
			arr_deleter<t>(pool, size)
		);
	}

	template<typename t,size_t size>
	std::shared_ptr<t[]> construct_share() {
		auto head = allocate<t,size>(this);

		for (size_t i = 0; i < size; i++)
		{
			new (&((t*)head)[i]) t();
		}

		return std::shared_ptr<t[]>(
			head,
			sarr_deleter<t,size>(pool)
		);
	}



	template<typename t>
	std::unique_ptr<t,deleter<t>> construct_uni() {
		return std::unique_ptr<t, deleter<t>>(
			new (allocate<t>(this)) t(),
			deleter<t>(pool)
		);
	}


	template<typename t>
	std::unique_ptr<t[], arr_deleter<t>> construct_uni(size_t size) {
		auto head = allocate<t>(this,size);

		for (size_t i = 0; i < size; i++)
		{
			new (&((t*)head)[i]) t();
		}

		return std::unique_ptr<t, deleter<t>>(
			head,
			arr_deleter<t>(pool, size)
		);
	}

	template<typename t,size_t size>
	std::unique_ptr<t[], sarr_deleter<t,size>> construct_uni() {
		auto head = allocate<t,size>(this);

		for (size_t i = 0; i < size; i++)
		{
			new (&((t*)head)[i]) t();
		}

		return std::unique_ptr<t[], sarr_deleter<t,size>>(
			head,
			sarr_deleter<t,size>(pool)
		);
	}
};

#include <list>
#include <map>
#include <typeindex>
#include <memory>
#include <source_location>

/// <summary>
/// gc有りメモリプール
/// </summary>
/// <typeparam name="chunk_size"></typeparam>
template<size_t chunk_size>
class anytype_pool {

public:
	template<size_t pool_size>
	struct chunk {
		
		//
		// type
		//
		
		struct typeinfo {
			size_t tsize = 0;
			void (*destruct)(void*) = nullptr;
			void (*move)(void*, void*) = nullptr;
		};

		struct info {
			void* ptr = nullptr;
			std::type_index tid = typeid(int);

			bool operator == (const info& other)const {
				return ptr == other.ptr;
			}
			bool operator < (const info& other)const {
				return ptr < other.ptr;
			}
		};

		struct deleter {
			typeinfo* type = nullptr;
			size_t* released = nullptr;
			deleter() = default;
			deleter(const deleter& d) = default;
			deleter(typeinfo& i,size_t& r) : type(&i),released(&r) {};

			void operator () (void* block)const {
				static_assert(std::is_standard_layout_v<info>, "ptrから元の構造体にキャストができなくなる");

				if(type->destruct) 
					type->destruct(((info*)block)->ptr);

				((info*)block)->ptr = nullptr;

				*released += type->tsize;
			}
		};

		template<typename t>
		using unique_ptr = std::unique_ptr<t*, deleter>;

		//
		// member
		//


		static inline constexpr size_t max_byte = pool_size;

		//型ごとの処理を持つmap 
		// 
		// これstaticでいい気もするけど後々のマルチスレッド対応を考えると困る
		//後々考えます
		std::map<std::type_index, typeinfo> typemap;
		
		// 変数の参照するポインタ群
		std::list<info> infos;
		
		//解放済みの未回収サイズ
		size_t released = 0;
		
		//最高尾byte
		size_t back_ind = 0;
		
		//アドレス昇順
		std::uint8_t data[max_byte];
		


		//デバッグメンバー
#ifdef _DEBUG 
		std::source_location _loc;
		
		chunk(const std::source_location& loc = std::source_location::current()) 
			:_loc(loc)
		{
			
		}
#endif 

		~chunk() {
			gc();

			if (infos.size()) {//この時点でメモリリークが確定する

				std::cerr << "========== memory leak ==========\n";

				void (*d)(void*);

				for (auto& i : infos)
				{
					std::cerr << "leak object :" << i.ptr << "\n";
					d = typemap[i.tid].destruct;

					if (d) d(i.ptr);
				}

#ifdef _DEBUG
				std::cerr << "\n\nby " << _loc.file_name() <<
					", line " << _loc.line() <<
					", col " << _loc.column() << "\n";
#endif
				std::cerr << "=================================" << std::endl;

			}
		}


		template<typename t, typename... types>
		unique_ptr<t> construct_unique(types&&... args) {

			auto new_ind = back_ind + sizeof(t);
			if (new_ind > max_byte) {
				return {};
			}

			new ((t*)&data[back_ind]) t(std::forward<types>(args)...);

			info tmp;
			tmp.ptr = (t*)&data[back_ind];

			back_ind = new_ind;

			auto& type = typemap[typeid(t)];


			//トリビアルな物は無用な為
			//ただ、関数のオーバーヘッドとifのオーバーヘッドどちらが大きいのかによって変わる為注意
			if constexpr (!std::is_trivially_copyable_v<t>) {
				type.destruct = &destruct_obj<t>;
				type.move = &move_obj<t>;
			}

			tmp.tid = typeid(t);
			type.tsize = sizeof(t);

			return unique_ptr<t>((t**)(char*)&infos.emplace_back(tmp), deleter(type, released));

		}

		template<typename t, typename... types>
		std::shared_ptr<t*> construct_shared(types&&... args) {

			auto new_ind = back_ind + (sizeof(t));
			if (new_ind > max_byte) {
				return {};
			}

			new ((t*)&data[back_ind]) t(std::forward<types>(args)...);

			info tmp;
			tmp.ptr = (t*)&data[back_ind];
			back_ind = new_ind;

			auto& type = typemap[typeid(t)];

			if constexpr (!std::is_trivially_copyable_v<t>) {
				type.destruct = &destruct_obj<t>;
				type.move = &move_obj<t>;
			}

			tmp.tid = typeid(t);
			type.tsize = sizeof(t);

			return std::shared_ptr<t*>((t**)(char*)&infos.emplace_back(tmp), deleter(type, released));
		}

		template<typename t>
		void destruct(unique_ptr<t>& ptr) {
			if (!ptr) return;

			info tmp;
			tmp.ptr = *ptr.get();

			auto itr = std::find(infos.begin(), infos.end(), tmp);
			if (itr == infos.end()) {
				throw error::located_exception("プールに含まれないオブジェクトの破棄");
			}
			ptr.reset();

			infos.erase(itr);
		}

		template<typename t>
		void destruct(std::shared_ptr<t*>& ptr) {
			if (!ptr) return;

			info tmp;
			tmp.ptr = *ptr.get();

			auto itr = std::find(infos.begin(), infos.end(), tmp);
			if (itr == infos.end()) {
				throw error::located_exception("プールに含まれないオブジェクトの破棄");
			}

			if (ptr.use_count() != 1) {
				throw error::located_exception("参照されているオブジェクトの破棄");
			}

			ptr.reset();

			infos.erase(itr);
		}

		void gc() {
			released = 0;

			std::erase_if(infos, [](const info& i) {
				return !i.ptr;
				});

			size_t offset = 0;
			infos.sort();

			typeinfo* tip = nullptr;

			//アドレス昇順
			for (auto& i : infos)
			{
				tip = &typemap[i.tid];

				if (tip->move) {
					tip->move(&data[offset], i.ptr);
					i.ptr = &data[offset];
				}
				else 
#ifdef _DEBUG
					if (&data[offset] != i.ptr)
#endif 
				{ 
					//moveコンストラクトがない場合トリビアル型
					std::memmove(&data[offset], i.ptr, tip->tsize);
					i.ptr = &data[offset];
				}

#ifdef _DEBUG
				else {//記録されているptrと計算結果のptrが合わない
					throw error::located_exception("ptrの復元に失敗");
				}
#endif 


				offset += tip->tsize;
			}

			//最後尾は有効なインスタンスサイズの合計
			back_ind = offset;
		}

		size_t dead_space()const {
			return released;
		}

		bool constructable(const size_t& byte) {
			return (back_ind + byte) <= max_byte;
		}

		bool is_contains(void* block) const {
			return data <= block && block <= &data[max_byte];
		}

		template<typename t>
		static void destruct_obj(void* block) {
			((t*)block)->~t();
		}
		template<typename t>
		static void move_obj(void* dest, void* src) {
			if (dest == src) return;

			auto to = (t*)dest;
			auto from = (t*)src;

			if (from - to < sizeof(t)) {

#ifdef _DEBUG
				if (from - to < 0) //okorimasenn youni
					throw error::located_exception("fatal error moveするアドレスの反転");
#endif 

				//詰める領域が被っている場合、一時バッファを仲介する
				t tmp(std::move(*from));
				
				from->~t();
				
				new (to) t(std::move(tmp));

			}
			else {
				new (to) t(std::move(*from));
				
				from->~t();
			}

		}
	};

	using chunk_type = typename chunk<chunk_size>;

	template<typename t>
	using unique_ptr_type = typename chunk_type::template unique_ptr <t>;

	template<typename t>
	using shared_ptr_type = typename std::shared_ptr<t>;

private:
	std::list<chunk_type> _chunks;
public:

#ifdef _DEBUG

private:
	std::source_location _loc;
public:
	anytype_pool(const std::source_location& loc = std::source_location::current())
		:_loc(loc)
	{

	}
#endif

	template<typename t, typename ... types>
	auto construct_unique(types&& ... args) {

		if (_chunks.empty()) {

#ifdef _DEBUG 
			_chunks.emplace_back(_loc);
#else
			_chunks.emplace_back();
#endif
		}

		for (auto& i : _chunks)
		{
			if (i.constructable(sizeof(t))) {
				return i.template construct_unique<t, types...>(std::forward<types>(args)...);
			}
			else if(i.dead_space() > sizeof(t) * 5){
 
				i.gc();
				if (i.constructable(sizeof(t))) {
					return i.template construct_unique<t, types...>(std::forward<types>(args)...);
				}
			}
		}

#ifdef _DEBUG 
		_chunks.emplace_back(_loc);
#else
		_chunks.emplace_back();
#endif
		auto& back = _chunks.back();

#ifdef _DEBUG
		if (!back.constructable(sizeof(t))) {

			throw error::located_exception("プールより大きなオブジェクトの確保");
		}
#endif

		return back.template construct_unique<t, types...>(std::forward<types>(args)...);
	}


	template<typename t, typename ... types>
	auto construct_shared(types&& ... args) {

		if (_chunks.empty()) {

#ifdef _DEBUG 
			_chunks.emplace_back(_loc);
#else
			_chunks.emplace_back();
#endif
		}

		for (auto& i : _chunks)
		{
			if (i.constructable(sizeof(t))) {
				return i.template construct_shared<t, types...>(std::forward<types>(args)...);
			}
			else if (i.dead_space() > sizeof(t) * 5) {

				i.gc();
				if (i.constructable(sizeof(t))) {
					return i.template construct_shared<t, types...>(std::forward<types>(args)...);
				}
			}
		}

#ifdef _DEBUG 
		_chunks.emplace_back(_loc);
#else
		_chunks.emplace_back();
#endif
		auto& back = _chunks.back();

#ifdef _DEBUG
		if (!back.constructable(sizeof(t))) {

			throw error::located_exception("プールより大きなオブジェクトの確保");
		}
#endif

		return back.template construct_shared<t, types...>(std::forward<types>(args)...);
	}
	

	template<typename t>
	void destruct(unique_ptr_type<t>& ptr) {

		for (auto& i : _chunks)
		{
			if (i.is_contains(*ptr.get())) {
				i.destruct<t>(ptr);
				return;
			}
		}

		throw error::located_exception("プールに存在しないオブジェクトの破棄");
	}

	void gc() {

		for (auto& i : _chunks)
		{
			i.gc();
		}
	}
};

/// <summary>
/// gc有り配列対応メモリプール
/// </summary>
/// <typeparam name="chunk_size"></typeparam>
template<size_t chunk_size>
class arraysup_anytype_pool {

public:
	template<size_t pool_size>
	struct chunk {
		
		struct typeinfo {
			size_t tsize = 0;
			void (*destruct)(void*, size_t) = nullptr;
			void (*move)(void*, void*, size_t) = nullptr;
		};

		struct info {
			void* ptr = nullptr;
			size_t num = 0;
			std::type_index tid = typeid(int);

			bool operator == (const info& other)const {
				return ptr == other.ptr;
			}
			bool operator < (const info& other)const {
				return ptr < other.ptr;
			}
		};

		struct deleter {
			typeinfo* type = nullptr;
			size_t* released = nullptr;
			deleter() = default;
			deleter(const deleter& d) = default;
			deleter(typeinfo& i, size_t& r) : type(&i), released(&r) {};

			void operator () (void* block)const {
				static_assert(std::is_standard_layout_v<info>, "ptrから元の構造体にキャストができなくなる");

				if (type->destruct)
					type->destruct(((info*)block)->ptr, ((info*)block)->num);

				((info*)block)->ptr = nullptr;

				*released += type->tsize;
			}
		};

		template<typename t>
		using unique_ptr = std::unique_ptr<t*, deleter>;

		static inline constexpr size_t max_byte = pool_size;

		std::map<std::type_index, typeinfo> typemap;
		std::list<info> infos;
		size_t released = 0;
		size_t back_ind = 0;
		std::uint8_t data[max_byte];
		//アドレス昇順

#ifdef _DEBUG
		std::source_location _loc;
		chunk(const std::source_location& loc = std::source_location::current()) 
			:_loc(loc)
		{

		}
#endif

		~chunk() {
			gc();

			if (infos.size()) {//この時点でメモリリークが確定する

				std::cerr << "========== memory leak ==========\n";

				void (*d)(void*, size_t);

				for (auto& i : infos)
				{
					std::cerr << "leak object :" << i.ptr << " , num " << i.num << "\n";
					d = typemap[i.tid].destruct;

					if (d) d(i.ptr, i.num);
				}

#ifdef _DEBUG
				std::cerr << "\n\nby " << _loc.file_name() <<
					", line " << _loc.line() <<
					", col " << _loc.column() << "\n";
#endif
				std::cerr << "=================================" << std::endl;

			}
		}


		template<typename t, typename... types>
		unique_ptr<t> construct_unique(size_t num,types&&... args) {

			auto new_ind = back_ind + (num * sizeof(t));
			if (new_ind > max_byte) {
				return {};
			}

			auto ptr = (t*)&data[back_ind];
			for (size_t i = 0; i < num; i++)
			{
				new (&ptr[i]) t(std::forward<types>(args)...);
			}

			back_ind = new_ind;

			auto& type = typemap[typeid(t)];

			if constexpr (!std::is_trivially_copyable_v<t>) {
				type.destruct = &destruct_obj<t>;
				type.move = &move_obj<t>;
			}

			info tmp;
			tmp.ptr = ptr;
			tmp.tid = typeid(t);
			tmp.num = num;
			type.tsize = sizeof(t);

			return unique_ptr<t>((t**)(char*)&infos.emplace_back(tmp), deleter(type, released));

		}

		template<typename t, typename... types>
		std::shared_ptr<t*> construct_shared(size_t num,types&&... args) {

			auto new_ind = back_ind + (num * sizeof(t));
			if (new_ind > max_byte) {
				return {};
			}

			auto ptr = (t*)&data[back_ind];
			for (size_t i = 0; i < num; i++)
			{
				new (&ptr[i]) t(std::forward<types>(args)...);
			}

			back_ind = new_ind;

			auto& type = typemap[typeid(t)];

			if constexpr (!std::is_trivially_copyable_v<t>) {
				type.destruct = &destruct_obj<t>;
				type.move = &move_obj<t>;
			}

			info tmp;
			tmp.ptr = ptr;
			tmp.tid = typeid(t);
			tmp.num = num;
			type.tsize = sizeof(t);

			return std::shared_ptr<t*>((t**)(char*)&infos.emplace_back(tmp), deleter(type, released));

		}

		template<typename t>
		void destruct(unique_ptr<t>& ptr) {
			if (!ptr) return;

			info tmp;
			tmp.ptr = *ptr.get();

			auto itr = std::find(infos.begin(), infos.end(), tmp);
			if (itr == infos.end()) {
				throw error::located_exception("プールに含まれないオブジェクトの破棄");
			}

			ptr.reset();

			infos.erase(itr);
		}

		template<typename t>
		void destruct(std::shared_ptr<t*>& ptr) {
			if (!ptr) return;

			info tmp;
			tmp.ptr = *ptr.get();

			auto itr = std::find(infos.begin(), infos.end(), tmp);
			if (itr == infos.end()) {
				throw error::located_exception("プールに含まれないオブジェクトの破棄");
			}

			if (ptr.use_count() != 1) {
				throw error::located_exception("参照されているオブジェクトの破棄");
			}

			ptr.reset();

			infos.erase(itr);
		}

		void gc() {
			released = 0;

			std::erase_if(infos, [](const info& i) {
				return !i.ptr;
				});

			size_t offset = 0;
			infos.sort();

			typeinfo* tip = nullptr;

			//アドレス昇順
			for (auto& i : infos)
			{
				tip = &typemap[i.tid];

				if (tip->move) {
					tip->move(&data[offset], i.ptr, i.num);
					i.ptr = &data[offset];
				}
				else if (&data[offset] != i.ptr) {
					std::memmove(&data[offset], i.ptr, tip->tsize * i.num);
					i.ptr = &data[offset];
				}

				offset += tip->tsize * i.num;
			}

			back_ind = offset;
		}

		size_t dead_space()const {
			return released;
		}

		bool constructable(const size_t& byte,const size_t& num) const{
			return (back_ind + (num * byte)) <= max_byte;
		}

		bool is_contains(void* block) const {
			return data <= block && block <= &data[max_byte];
		}

		template<typename t>
		static void destruct_obj(void* block, size_t num) {
			for (size_t i = 0; i < num; i++)
			{
				((t*)block)[i].~t();
			}
		}
		template<typename t>
		static void move_obj(void* dest, void* src, size_t num) {
			if (dest == src) return;

			auto to = (t*)dest;
			auto from = (t*)src;

			if (from - to < sizeof(t)) {

#ifdef _DEBUG
				if (from - to < 0)
					throw error::located_exception("fatal error moveするアドレスの反転");
#endif 

				//詰める領域が被っている場合、一時バッファを仲介する
				for (size_t i = 0; i < num; i++)
				{
					t tmp(std::move(from[i]));
				
					from[i].~t();
				
					new (&to[i]) t(std::move(tmp));
				}
			}
			else {

				for (size_t i = 0; i < num; i++)
				{
					new (&to[i]) t(std::move(from[i]));
				
					from[i].~t();
				}
			}

		}
	};

	using chunk_type = typename chunk<chunk_size>;

	template<typename t>
	using unique_ptr_type = typename chunk_type::template unique_ptr <t>;

	template<typename t>
	using shared_ptr_type = typename std::shared_ptr<t*>;

private:
	std::list<chunk_type> _chunks;
public:

#ifdef _DEBUG
private:
	std::source_location _loc;
public:
	arraysup_anytype_pool(const std::source_location& loc = std::source_location::current())
		:_loc(loc)
	{

	}
#endif

	template<typename t, typename ... types>
	auto construct_unique(size_t num,types&& ... args) {

		if (_chunks.empty()) {
#ifdef _DEBUG
			_chunks.emplace_back(_loc);
#else
			_chunks.emplace_back();
#endif
		}
			


		for (auto& i : _chunks)
		{
			if (i.constructable(sizeof(t), num)) {
				return i.template construct_unique<t, types...>(num, std::forward<types>(args)...);
			}
			else if (i.dead_space() > sizeof(t) * 5) {

				i.gc();
				if (i.constructable(sizeof(t), num)) {
					return i.template construct_unique<t, types...>(num, std::forward<types>(args)...);
				}
			}
		}
#ifdef _DEBUG
		_chunks.emplace_back(_loc);
#else
		_chunks.emplace_back();
#endif
		auto& back = _chunks.back();

#ifdef _DEBUG
		if (!back.constructable(sizeof(t), num)) {

			throw error::located_exception("プールより大きなオブジェクトの確保");
		}
#endif

		return back.template construct_unique<t, types...>(num, std::forward<types>(args)...);
	}

	template<typename t, typename ... types>
	auto construct_shared(size_t num, types&& ... args) {

		if (_chunks.empty()) {
#ifdef _DEBUG
			_chunks.emplace_back(_loc);
#else
			_chunks.emplace_back();
#endif
		}

		for (auto& i : _chunks)
		{
			if (i.constructable(sizeof(t), num)) {
				return i.template construct_shared<t, types...>(num, std::forward<types>(args)...);
			}
			else if (i.dead_space() > sizeof(t) * 5) {

				i.gc();
				if (i.constructable(sizeof(t)), num) {
					return i.template construct_shared<t, types...>(num, std::forward<types>(args)...);
				}
			}
		}
#ifdef _DEBUG
		_chunks.emplace_back(_loc);
#else
		_chunks.emplace_back();
#endif
		auto& back = _chunks.back();

#ifdef _DEBUG
		if (!back.constructable(sizeof(t), num)) {

			throw error::located_exception("プールより大きなオブジェクトの確保");
		}
#endif

		return back.template construct_shared<t, types...>(num, std::forward<types>(args)...);
	}


	template<typename t>
	void destruct(unique_ptr_type<t>& ptr) {

		for (auto& i : _chunks)
		{
			if (i.is_contains(*ptr.get())) {
				i.destruct<t>(ptr);
				return;
			}
		}

		throw error::located_exception("プールに存在しないオブジェクトの破棄");
	}

	void gc() {

		for (auto& i : _chunks)
		{
			i.gc();
		}
	}
};