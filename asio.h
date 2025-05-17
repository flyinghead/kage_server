#pragma once
#include <asio.hpp>

#undef ECHO

#if ASIO_VERSION < 102900
namespace asio::placeholders
{
static inline constexpr auto& error = std::placeholders::_1;
static inline constexpr auto& bytes_transferred = std::placeholders::_2;
}
#endif
