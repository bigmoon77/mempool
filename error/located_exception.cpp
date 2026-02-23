#include "located_exception.h"
#include <format>

error::located_exception::located_exception(
	const std::string& str,
	const std::source_location& location)
	:exception(
		std::format<const char*, const char*, std::uint_least32_t, std::uint_least32_t>(
			"message {}, file {}, line {}, column {}",
			str.c_str(),
			location.file_name(),
			location.line(),
			location.column()
		).c_str()
	)
{

}