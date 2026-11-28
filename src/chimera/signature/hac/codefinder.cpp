// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>
#include "codefinder.h"
#include "codefinder_v2.hpp"

// Legacy CodeFinder interface, kept so existing callers (signature.cpp, FindCode) do not need
// to change. The scanning itself is now done by CodeFinderV2, which replaces the old
// implementation that had these problems:
//   - boyerFind() read signature[-1] after a match at offset 0 (out-of-bounds read)
//   - the wildcard scan kept reading signature[size] and memory past a match (out-of-bounds read)
//   - SizeOfRawData - size could underflow on small sections
//   - only the first executable section was scanned
//   - an invalid signature byte called std::exit(12345), killing the game
// An invalid signature now simply yields no matches, so the feature is reported as missing.

CodeFinder::CodeFinder(HANDLE module, const short* signature, unsigned int signatureLen) {
	this->module = module;
	this->signature = signature;
	this->length = signatureLen;
}

std::vector<std::uintptr_t>::iterator CodeFinder::begin() {
	find();
	return locations.begin();
}

std::vector<std::uintptr_t>::iterator CodeFinder::end() {
	return locations.end();
}

std::vector<std::uintptr_t> CodeFinder::find() {
	locations.clear();

	if(!module || !signature || length == 0) {
		return locations;
	}

	const Chimera::CodeFinderV2 finder(reinterpret_cast<HMODULE>(module), signature, length);
	if(finder.valid()) {
		locations = finder.find_all();
	}

	return locations;
}

std::uintptr_t FindCode(HANDLE module, const short* signature, size_t signatureLen, unsigned int match_num = 0) {
	CodeFinder finder(module, signature, signatureLen);
	std::vector<std::uintptr_t> locations = finder.find();

	if(!locations.empty() and match_num < locations.size()) {
		return locations[match_num];
	} else {
		return (std::uintptr_t) NULL;
	}
}
