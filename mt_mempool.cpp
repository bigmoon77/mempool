#include "mt_mempool.h"

mempool_util::pointer_mutex& mempool_util::get_pointer_mutex()
{
	static pointer_mutex mtx;
	return mtx;
}