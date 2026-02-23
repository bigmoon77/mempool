#pragma once
#include <exception>
#include <source_location>
#include <string>

namespace error {

	class located_exception : public std::exception
	{

	public:

		located_exception(
			const std::string& str,
			const std::source_location& location = std::source_location::current()
		);

	};

};
