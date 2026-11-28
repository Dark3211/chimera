#pragma once

#ifndef _CODEFINDER_H
#define _CODEFINDER_H

#include <windows.h>
#include <cstdint>
#include <vector>

class CodeFinder {
	HANDLE module;
	unsigned int length;
	const short* signature;
	std::vector<std::uintptr_t> locations;

public:
	CodeFinder(HANDLE module, const short* signature, unsigned int signatureLen);
	std::vector<std::uintptr_t>::iterator begin();
	std::vector<std::uintptr_t>::iterator end();
	std::vector<std::uintptr_t> find();
};

//Old interface so I don't have to change all of the hooks, for now
std::uintptr_t FindCode(HANDLE module, const short* signature, size_t signatureLen, unsigned int match_num);

#endif
