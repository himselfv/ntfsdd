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


void BitmapSpans::Iterator::findFirst() {
	if ((*ptr & 1ULL) == 0)
		this->skip0s();
	if (this->ptr)
		this->eat1s();
}

//On entrance, must point to 0.
//Skips the 0s. On exit, either points to 1 or nulls the ptr.
void BitmapSpans::Iterator::skip0s() {
	if (!this->ptr) return;
	current.offset += current.length; //Roll the previous length into the position.
	current.length = 0;
	size_t bit_offset = current.offset % BLOCK_BITS;

	//Finish the open word
	uint64_t word = *ptr >> bit_offset;
	while (bit_offset < BLOCK_BITS - 1 && (size_t)current.offset < size) {
		bit_offset++;
		current.offset++;
		word = word >> 1;
		if (word & 1ULL) return;
	}

	//Don't check current.offset < size here, next block is gonna roll this case in safely.

	//Next go full words
	current.offset++;
	ptr++;
	while ((size_t)current.offset < size && *ptr == 0) {
		ptr++;
		current.offset += BLOCK_BITS;
	};

	//Have to check before dereferencing ptr.
	if ((size_t)current.offset >= size) {
		this->ptr = nullptr;
		return;
	}

	//Now do the non-0 block
	word = *ptr;
	while ((size_t)current.offset < size && ((word & 1ULL) == 0)) {
		current.offset++;
		word = word >> 1;
	}
	if ((size_t)current.offset >= size)
		this->ptr = nullptr;
}

//On entrance, must point to 1.
//Reads the 1s until hitting either 0 or EOF. Does not null the ptr, allowing this to be a hit. Next skip0s() is going to do that.
void BitmapSpans::Iterator::eat1s() {
	size_t bit_offset = current.offset % BLOCK_BITS;
	LCN remainingSize = size - current.offset;

	//Finish the open word
	uint64_t word = *ptr >> bit_offset;
	while (bit_offset < BLOCK_BITS - 1 && current.length < remainingSize) {
		bit_offset++;
		current.length++;
		word = word >> 1;
		if ((word & 1ULL) == 0) return;
	}

	//Don't check current.offset < size here, next block is gonna roll this case in safely.

	//Next go full words
	current.length++;
	ptr++;
	while (current.length < remainingSize && *ptr == static_cast<uint64_t>(-1)) {
		ptr++;
		current.length += BLOCK_BITS;
	};

	//Have to check before dereferencing ptr.
	if (current.length >= remainingSize) {
		this->ptr = nullptr;
		return;
	}

	//Now do the non-0 block
	word = *ptr;
	while (current.length < remainingSize && ((word & 1ULL) == 1)) {
		current.length++;
		word = word >> 1;
	}
	if (current.length >= remainingSize)
		this->ptr = nullptr;
}

