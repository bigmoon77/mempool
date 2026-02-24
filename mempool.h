#pragma once

#include <located_exception.h>
#include <list>


template<typename t, size_t pool_size>
class unitype_pool {
	static inline constexpr size_t max_count = pool_size;
public:
	using value_type = t;
private:

	struct chunk {

#pragma region データ構造
		struct info {
			value_type* ptr = nullptr;
			size_t num = 0;

			bool operator == (const info& other) const {
				return ptr == other.ptr;
			}
			bool operator < (const info& other)const {
				return ptr < other.ptr;
			}
		};


		class proxy_ptr {
			info* _ptr = nullptr;
		public:
			proxy_ptr() = default;
			proxy_ptr(info& ref) : _ptr(&ref) {
			}
			proxy_ptr(const proxy_ptr& other) :_ptr(other._ptr) {
			}

			~proxy_ptr() = default;

			value_type* operator -> () {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->ptr;
			}
			const value_type* operator -> ()const {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->ptr;
			}

			value_type& operator*() {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return *_ptr->ptr;
			}
			const value_type& operator *()const {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return *_ptr->ptr;
			}

			value_type* get() {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->ptr;
			}
			const value_type* get()const {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->ptr;
			}

			value_type& operator [](const size_t& ind) {

#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->ptr[ind];
			}

			const value_type& operator [](const size_t& ind) const {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->ptr[ind];
			}

			proxy_ptr& operator = (const proxy_ptr& other) {
				//参照するポインタの代入　poolでのリークに注意
				_ptr = other._ptr;
				return *this;
			}

			const size_t& get_size() const {
#ifdef _DEBUG
				if (!_ptr)throw error::located_exception("nullptr");
#endif
				return _ptr->num;
			}

			operator bool() const {
				return _ptr && (_ptr->ptr);
			}
		};

#pragma endregion

		size_t inst_count = 0;
		size_t back_ind = 0;

		std::uint8_t data[sizeof(value_type) * max_count];

		//setの方が良いのだが、stlを使用したくないニーズがあると思ったので
		//使用者はこの配列要素のptrを参照し、メモリの移動に対応する
		info infos[max_count] = {};



		chunk() = default;

		~chunk() {

			for (size_t i = 0; i < max_count; i++)
			{
				if (infos[i].ptr) {
					for (size_t j = 0; j < infos[i].num; j++)
					{
						infos[i].ptr[j].~value_type();
					}


					std::cerr << "memory leak " << infos[i].ptr << "[" << infos[i].num << "]" << std::endl;
				}
			}

		}



		template<typename... types>
		proxy_ptr construct(types&& ... args, const size_t& num = 1) {

			auto new_ind = back_ind + num;
			if (new_ind > max_count) {
				return {};
			}

			value_type* res = (value_type*)&data[back_ind * sizeof(value_type)];

			for (size_t i = 0; i < num; i++)
			{
				new (&res[i]) value_type(std::forward<types>(args)...);
			}

			back_ind = new_ind;
			inst_count += num;

			//find ptr
			size_t i = 0;
			for (; i < max_count; i++)
			{
				if (!infos[i].ptr) {
					infos[i].ptr = res;
					infos[i].num = num;
					break;
				}
			}

			return proxy_ptr(infos[i]);
		}

		void destruct(value_type* ptr, const size_t& num = 1) {
#ifdef _DEBUG
			if (!(is_contains(ptr) && is_contains(ptr + (num - 1)))) {
				throw error::located_exception("bad ptr");
			}
#endif // _DEBUG
			for (size_t i = 0; i < num; i++)
			{
				ptr[i].~value_type();
			}

			if (is_back(&ptr[num - 1])) {
#ifdef _DEBUG
				if (back_ind < num) {
					throw error::located_exception("bad dealloc num");
				}
#endif // _DEBUG
				back_ind -= num;
			}

			inst_count -= num;

			//find ptr
			for (size_t i = 0; i < max_count; i++)
			{
				if (infos[i].ptr == ptr) {
					infos[i].ptr = nullptr;
					infos[i].num = 0;
					break;
				}
			}
		}

		void destruct(proxy_ptr& ptr) {
#ifdef _DEBUG
			if (!(is_contains(ptr.get()) && is_contains(ptr.get() + (ptr.get_size() - 1)))) {
				throw error::located_exception("bad ptr");
			}
#endif // _DEBUG
			for (size_t i = 0; i < ptr.get_size(); i++)
			{
				ptr[i].~value_type();
			}

			if (is_back(&ptr[ptr.get_size() - 1])) {
#ifdef _DEBUG
				if (back_ind < ptr.get_size()) {
					throw error::located_exception("bad dealloc num");
				}
#endif // _DEBUG
				back_ind -= ptr.get_size();
			}

			inst_count -= ptr.get_size();

			//find ptr
			for (size_t i = 0; i < max_count; i++)
			{
				if (infos[i].ptr == ptr.get()) {
					infos[i].ptr = nullptr;
					infos[i].num = 0;
					break;
				}
			}
		}

		bool is_contains(value_type* ptr) const {

			return back_ind && data <= (std::uint8_t*)ptr && (std::uint8_t*)ptr <= &data[(back_ind - 1) * sizeof(value_type)];
		}

		bool is_back(value_type* ptr) const {
			return ptr == (value_type*)&data[(max_count - 1) * sizeof(value_type)];
		}


		float mem_efficiency() {
			if (!back_ind)
				return 0;
			return static_cast<float>(inst_count) / static_cast<float>(back_ind);
		}

		bool constructable(const size_t& num = 1) {
			return (back_ind + num <= max_count);
		}


		void gc() {

			//未使用な領域がない為不要
			if (back_ind == inst_count) return;


			std::unique_ptr<info[]> tmp(new info[max_count]);
			std::memcpy(tmp.get(), infos, sizeof(info) * max_count);

			///アドレス昇順
			std::qsort(tmp.get(), max_count, sizeof(info), &compare);

			size_t offset = 0;

			auto arr = tmp.get();
			for (size_t i = 0; i < max_count; i++)
			{
				if (arr[i].ptr) {

					//動かしたオブジェクトの参照の更新

					if constexpr (std::is_trivially_copyable_v<value_type>) {
						//メモリの直接操作

						std::memmove(
							&data[offset],
							arr[i].ptr,
							sizeof(value_type) * arr[i].num
						);

						size_t ind = 0;
						for (; ind < max_count; ind++)
						{
							if (infos[ind].ptr == arr[i].ptr)
								break;
						}

						infos[ind].ptr = (value_type*)&data[offset];
					}
					else {
						//メモリを直接動かせない場合はmove constructorで代用する
						auto ptr = (value_type*)&data[offset];
						for (size_t ind = 0; ind < arr[i].num; ind++)
						{
							new (&ptr[ind]) value_type(std::move(arr[i].ptr[ind]));
							arr[i].ptr[ind].~value_type();//move後は終了
						}

						size_t ind = 0;
						for (; ind < max_count; ind++)
						{
							if (infos[ind].ptr == arr[i].ptr)
								break;
						}
						infos[ind].ptr = ptr;
					}

					offset += arr[i].num * sizeof(value_type);
				}
			}
			back_ind = offset / sizeof(value_type);

		}

		static int compare(const void* a, const void* b) {
			return static_cast<int>(
				std::max<long long>(std::numeric_limits<int>::min(),//怖いからオーバーフロー対策
					std::min<long long>(std::numeric_limits<int>::max(), ((info*)a)->ptr - ((info*)b)->ptr)
				));
		}
	};

	std::list<chunk> _chunks;

public:
	template<typename ... types>
	auto construct(types&&... args, const size_t& num = 1) {
		if (_chunks.empty()) {
			_chunks.emplace_back();
		}


		for (auto& i : _chunks)
		{
			if (i.constructable(num)) {
				return i.construct(std::forward<types>(args)..., num);
			}
			else {
				i.gc();
				if (i.constructable(num)) {
					return i.construct(std::forward<types>(args)..., num);
				}
			}
		}

		_chunks.emplace_back();
		auto& back = _chunks.back();

#ifdef _DEBUG
		if (back.constructable(num)) {
			throw error::located_exception("プールより大きなオブジェクトの確保");
		}
#endif 
		return back.construct(std::forward<types>(args)..., num);
	}

	void destruct(chunk::proxy_ptr& ptr) {
		for (auto& i : _chunks)
		{
			if (i.is_contains(ptr.get())) {
				i.destruct(ptr);
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



template<size_t chunk_size>
class anytype_pool {

public:
	template<size_t pool_size>
	struct chunk {
		static inline constexpr size_t max_byte = pool_size;

		struct info {
			void* ptr = nullptr;
			size_t num = 0;
			size_t tsize = 0;
			void (*destruct)(void*, size_t) = nullptr;
			void (*move)(void*, void*, size_t) = nullptr;

			bool operator == (const info& other)const {
				return ptr == other.ptr;
			}
			bool operator < (const info& other)const {
				return ptr < other.ptr;
			}
		};

		template<typename t>
		class proxy_ptr {
		public:
			using value_type = t;
		private:
			info* _ptr = nullptr;
		public:
			proxy_ptr() = default;
			proxy_ptr(info& a) :_ptr(&a) {};
			proxy_ptr(const proxy_ptr& other) : _ptr(other._ptr) {};
			~proxy_ptr() = default;

			value_type* operator ->() {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif

				return (value_type*)_ptr->ptr;
			}
			const value_type* operator ->() const {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif
				return (value_type*)_ptr->ptr;
			}


			value_type& operator *() {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif

				return *(value_type*)_ptr->ptr;
			}
			const value_type& operator *() const {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif
				return *(value_type*)_ptr->ptr;
			}


			value_type& operator [](size_t ind) {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif

				return ((value_type*)_ptr->ptr)[ind];
			}
			const value_type& operator [](size_t ind) const {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif
				return ((value_type*)_ptr->ptr)[ind];
			}


			value_type* get() {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif

				return (value_type*)_ptr->ptr;
			}
			const value_type* get() const {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif
				return (value_type*)_ptr->ptr;
			}


			const size_t& get_size()const {
#ifdef _DEBUG
				if (!_ptr) {
					throw error::located_exception("nullptr");
				}
#endif
				return _ptr->num;
			}


			proxy_ptr& operator = (const proxy_ptr& other) {
				_ptr = other._ptr;
				return *this;
			}
		};



		std::uint8_t data[max_byte];

		//アドレス昇順
		std::list<info> infos;

		size_t back_ind = 0;


		~chunk() {
			for (auto& i : infos)
			{
				std::cerr << "memory leak " << i.ptr << "[" << i.num << "]" << std::endl;
				if (i.destruct)
					i.destruct(i.ptr, i.num);
			}
		}


		template<typename t, typename... types>
		proxy_ptr<t> construct(types&&... args, const size_t& num) {

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

			info tmp;
			if constexpr (std::is_trivially_copyable_v<t>) {

				tmp.ptr = ptr;
				tmp.num = num;
				tmp.tsize = sizeof(t);

			}
			else {

				tmp.ptr = ptr;
				tmp.num = num;
				tmp.tsize = sizeof(t);
				tmp.destruct = &destruct_obj<t>;
				tmp.move = &move_obj<t>;
			}

			return proxy_ptr<t>(infos.emplace_back(tmp));

		}

		template<typename t>
		void destruct(proxy_ptr<t>& ptr) {
			info tmp;
			tmp.ptr = ptr.get();

			auto itr = std::find(infos.begin(), infos.end(), tmp);
			if (itr == infos.end()) {
				throw error::located_exception("プールに含まれないオブジェクトの破棄");
			}

			if (itr->destruct) {
				itr->destruct(itr->ptr, itr->num);
			}//そうでない場合はトリビアル型なので破棄不要

			infos.erase(itr);
		}

		void gc() {

			size_t offset = 0;

			infos.sort();

			//アドレス昇順
			for (auto& i : infos)
			{
				if (i.move) {
					i.move(&data[offset], i.ptr, i.num);
					i.ptr = &data[offset];
				}
				else if (&data[offset] != i.ptr) {
					std::memmove(&data[offset], i.ptr, i.tsize * i.num);
					i.ptr = &data[offset];
				}

				offset += i.tsize * i.num;
			}

			back_ind = offset;
		}


		bool constructable(const size_t& byte, const size_t& num) {
			return back_ind + (num * byte) <= max_byte;
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
	using ptr_type = typename chunk_type::template proxy_ptr <t>;

private:
	std::list<chunk_type> _chunks;

public:

	template<typename t,typename ... types>
	auto construct(types&& ... args,const size_t& num) {
		
		if(_chunks.empty())
			_chunks.emplace_back();


		for (auto& i : _chunks)
		{
			if (i.constructable(sizeof(t), num)) {
				return i.construct<t,types...>(std::forward<types>(args)...,num);
			}
			else {
				i.gc();
				if (i.constructable(sizeof(t), num)) {
					return i.construct<t,types...>(std::forward<types>(args)...,num);
				}
			}
		}

		_chunks.emplace_back();
		auto& back = _chunks.back();

		if (back.constructable(sizeof(t), num)) {
			return back.construct<t>(num);
		}

		throw error::located_exception("プールより大きなオブジェクトの確保");

	}

	template<typename t>
	void destruct(ptr_type<t>& ptr) {

		for (auto& i : _chunks)
		{
			if (i.is_contains(ptr.get())) {
				i.destruct<t>(ptr);
				return;
			}
		}

		throw error::located_exception("プールに存在しないオブジェクトの破棄");
	}

	void gc() {

		for (auto& i :_chunks)
		{
			i.gc();
		}
	}
};
