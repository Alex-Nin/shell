#pragma once
#include <string>
#include <vector>

struct Parsed_Tokens
{
	std::string command;
	std::vector<std::string> args;
	std::string redirect;
	std::string redirect_symbol;

	Parsed_Tokens(const std::string& c, std::vector<std::string> a, const std::string& re, const std::string& rs) 
		: command(c), args(a), redirect(re), redirect_symbol(rs) {}
};