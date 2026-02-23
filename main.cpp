#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>



#include <iostream>
#include "mempool.h"

int main() {
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	unitype_pool<int,100> pool;


	auto obj = pool.construct<>(1);

	auto obj0 = pool.construct<>(1);
	auto obj1 = pool.construct<>(1);
	auto obj2 = pool.construct<>(1);
	auto obj3 = pool.construct<>(1);

	pool.destruct(obj);
	pool.destruct(obj2);
	for (size_t i = 0; i < 1; i++)
	{
		obj0[i] = 159;
		std::cout << obj0[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		obj1[i] = 753;
		std::cout << obj1[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		obj3[i] = 456;
		std::cout << obj3[i] << std::endl;
	}

	pool.gc();

	auto obj4 = pool.construct<>(1);
	auto obj5 = pool.construct<>(1);
	auto obj6 = pool.construct<>(1);

	pool.gc();
	for (size_t i = 0; i < 1; i++)
	{
		std::cout << obj0[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		std::cout << obj1[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		std::cout << obj3[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		std::cout << obj4[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		std::cout << obj5[i] << std::endl;
	}
	for (size_t i = 0; i < 1; i++)
	{
		std::cout << obj6[i] << std::endl;
	}

	return 0;
}