#pragma once
#include <vector>
#include "ntfsutil.h"

//Bitmap access to an externally-managed data.
struct Bitmap {
public:
	constexpr static size_t BLOCK_BITS = sizeof(uint64_t) * 8;
	uint64_t* data = nullptr;
	int64_t size = 0; //Size in bits
	Bitmap() {};
	Bitmap(void* data) : data(static_cast<uint64_t*>(data)) {};
	inline bool get(size_t idx) { return ((uint8_t*)data)[idx / 8] & (1 << (idx % 8)); }
	void set(size_t lo, size_t hi);
	void clear(size_t lo, size_t hi);
	void clear_all();
};

//Automatically managed buffer with bitmap access.
struct BitmapBuf : public Bitmap {
public:
	std::vector<uint8_t> buffer;
	BitmapBuf() {}
	BitmapBuf(size_t size);
	void resize(size_t size);
};