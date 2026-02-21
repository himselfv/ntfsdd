#pragma once
#include "bitmap.h"

void Bitmap::set(size_t lo, size_t hi) {
	if (lo > hi) return;
	assert(hi < size);

	size_t start_word = lo / BLOCK_BITS;
	size_t end_word = hi / BLOCK_BITS;
	size_t start_bit = lo % BLOCK_BITS;
	size_t end_bit = hi % BLOCK_BITS;

	// The entire range is within a single 64-bit word
	if (start_word == end_word) {
		uint64_t mask = (~0ULL << start_bit) & (~0ULL >> (BLOCK_BITS - 1 - end_bit));
		data[start_word] |= mask;
		return;
	}

	// Range spans multiple words
	data[start_word] |= (~0ULL << start_bit);
	if (end_word > start_word + 1)
		std::memset(&data[start_word + 1], 0xFF, (end_word - start_word - 1) * sizeof(uint64_t));
	data[end_word] |= (~0ULL >> (BLOCK_BITS - 1 - end_bit));
}

void Bitmap::clear(size_t lo, size_t hi) {
	if (lo > hi) return;
	assert(hi < size);

	size_t start_word = lo / BLOCK_BITS;
	size_t end_word = hi / BLOCK_BITS;
	size_t start_bit = lo % BLOCK_BITS;
	size_t end_bit = hi % BLOCK_BITS;

	if (start_word == end_word) {
		uint64_t mask = (~0ULL << start_bit) & (~0ULL >> (BLOCK_BITS - 1 - end_bit));
		data[start_word] &= ~mask;
		return;
	}

	// Range spans multiple words
	data[start_word] &= ~(~0ULL << start_bit);
	if (end_word > start_word + 1)
		std::memset(&data[start_word + 1], 0x00, (end_word - start_word - 1) * sizeof(uint64_t));
	data[end_word] &= ~(~0ULL >> (BLOCK_BITS - 1 - end_bit));
}
void Bitmap::clear_all() {
	this->clear(0, size - 1);
}

BitmapBuf::BitmapBuf(size_t size) {
	this->resize(size);
}

void BitmapBuf::resize(size_t size) {
	buffer.resize((size + 7) / 8);
	this->data = static_cast<uint64_t*>((void*)(buffer.data()));
	this->size = buffer.size() * 8;
}
