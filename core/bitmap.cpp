#pragma once
#include "bitmap.h"
#include <iostream>
#include <fstream>
#include "util.h"

//Bits are stored in low-bits-first order
//All masks here are inclusive of both sides.
#define MASK_BITSUNTIL(end_bit) (~0ULL >> (BLOCK_BITS - 1 - (end_bit)))
#define MASK_BITSAFTER(start_bit) (~0ULL << (start_bit))
#define MASK_BITSBETWEEN(L,R) MASK_BITSAFTER(L) & MASK_BITSUNTIL(R)


void Bitmap::set(size_t lo, size_t hi) {
	if (lo > hi) return;
	assert_lt(hi, size);

	size_t start_word = lo / BLOCK_BITS;
	size_t end_word = hi / BLOCK_BITS;
	size_t start_bit = lo % BLOCK_BITS;
	size_t end_bit = hi % BLOCK_BITS;

	// The entire range is within a single 64-bit word
	if (start_word == end_word) {
		uint64_t mask = MASK_BITSBETWEEN(start_bit, end_bit);
		data[start_word] |= mask;
		return;
	}

	// Range spans multiple words
	data[start_word] |= MASK_BITSAFTER(start_bit);
	if (end_word > start_word + 1)
		std::memset(&data[start_word + 1], 0xFF, (end_word - start_word - 1) * sizeof(uint64_t));
	data[end_word] |= MASK_BITSUNTIL(end_bit);
}

void Bitmap::clear(size_t lo, size_t hi) {
	if (lo > hi) return;
	assert(hi < size);

	size_t start_word = lo / BLOCK_BITS;
	size_t end_word = hi / BLOCK_BITS;
	size_t start_bit = lo % BLOCK_BITS;
	size_t end_bit = hi % BLOCK_BITS;

	if (start_word == end_word) {
		uint64_t mask = MASK_BITSBETWEEN(start_bit, end_bit);
		data[start_word] &= ~mask;
		return;
	}

	// Range spans multiple words
	data[start_word] &= ~MASK_BITSAFTER(start_bit);
	if (end_word > start_word + 1)
		std::memset(&data[start_word + 1], 0x00, (end_word - start_word - 1) * sizeof(uint64_t));
	data[end_word] &= ~MASK_BITSUNTIL(end_bit);
}
void Bitmap::clear_all() {
	this->clear(0, size - 1);
}


//Compares two bitmaps handling some edge cases. Returns 0 on equality!
//Offsets must start at a BLOCK_BITS boundary.
int64_t Bitmap::memcmp(const void* bitmap1, const void* bitmap2, size_t bitcnt, size_t offset1, size_t offset2)
{
	if (offset1 >= 8) {
		bitmap1 = (void*)((char*)bitmap1 + (offset1 / 8));
		offset1 = offset1 % 8;
	}
	if (offset2 >= 8) {
		bitmap2 = (void*)((char*)bitmap2 + (offset2 / 8));
		offset2 = offset2 % 8;
	}
	auto rem = bitcnt % 8;
	bitcnt = bitcnt / 8;
	auto ret = std::memcmp(bitmap1, bitmap2, bitcnt);
	if (ret != 0 || rem == 0) return ret;

	auto mask = MASK_BITSUNTIL(rem-1);
	return (((char*)bitmap1)[bitcnt] & rem) - (((char*)bitmap2)[bitcnt] & rem);
}


template <typename Op64>
void Bitmap::apply_operation1(Op64 op64)
{
	uint64_t* srcA = this->data;
	size_t rem = this->size;
	while (rem >= BLOCK_BITS) {
		if (!op64(srcA)) break;
		rem -= BLOCK_BITS;
		srcA++;
	}
	if (rem > 0) {
		uint64_t tmpA = *srcA & MASK_BITSUNTIL(rem-1);
		uint64_t tmpA2 = tmpA;
		op64(&tmpA2);
		if (tmpA2 != tmpA)
			*srcA = (*srcA & ~MASK_BITSUNTIL(rem-1)) | (tmpA2 & MASK_BITSUNTIL(rem-1));
	}
}

template <typename Op64>
void Bitmap::apply_operation1(Op64 op64, size_t first, size_t last)
{
	size_t skip = first / BLOCK_BITS;
	uint64_t* srcA = this->data + skip;

	first -= skip * BLOCK_BITS;
	last -= skip * BLOCK_BITS;

	if (first <= 0) {
		//Initial partial or full block starting at 0.
		//If it's partial, we'll treat it as a final partial.
	} else if (last < BLOCK_BITS) {
		//The only one partial block
		uint64_t tmpA = *srcA & MASK_BITSBETWEEN(first, last);
		uint64_t tmpA2 = tmpA;
		op64(&tmpA2);
		if (tmpA2 != tmpA)
			*srcA = (*srcA & ~MASK_BITSBETWEEN(first, last)) | (tmpA2 & MASK_BITSBETWEEN(first, last));
		srcA++;
		first = last + 1;
	} else {
		//Initial partial + more later
		uint64_t tmpA = *srcA & MASK_BITSAFTER(first);
		uint64_t tmpA2 = tmpA;
		op64(&tmpA2);
		if (tmpA2 != tmpA)
			*srcA = (*srcA & ~MASK_BITSAFTER(first)) | (tmpA2 & MASK_BITSAFTER(first));
		srcA++;
		first = BLOCK_BITS;
	}

	size_t rem = last - first + 1;
	while (rem >= BLOCK_BITS) {
		if (!op64(srcA)) break;
		rem -= BLOCK_BITS;
		srcA++;
	}
	if (rem > 0) { //Careful: bits 0..rem-1
		uint64_t tmpA = *srcA & MASK_BITSUNTIL(rem-1);
		uint64_t tmpA2 = tmpA;
		op64(&tmpA2);
		if (tmpA2 != tmpA)
			*srcA = (*srcA & ~MASK_BITSUNTIL(rem-1)) | (tmpA2 & MASK_BITSUNTIL(rem-1));
	}
}

template <typename Op64>
void Bitmap::apply_operation3(const Bitmap& other, Bitmap& result, Op64 op64) const
{
	uint64_t* srcA = this->data;
	uint64_t* srcB = other.data;
	uint64_t* dest = result.data;
	size_t rem = this->size;
	while (rem >= BLOCK_BITS) {
		op64(srcA, srcB, dest);
		rem -= BLOCK_BITS;
		srcA++;
		srcB++;
		dest++;
	}
	if (rem > 0) {
		uint64_t tmpA = *srcA & MASK_BITSUNTIL(rem-1);
		uint64_t tmpB = *srcB & MASK_BITSUNTIL(rem-1);
		uint64_t tmpDest = *dest & MASK_BITSUNTIL(rem-1);
		uint64_t tmpDest2 = tmpDest;
		op64(&tmpA, &tmpB, &tmpDest2);
		if (tmpDest != tmpDest2)
			*dest = (*dest & ~MASK_BITSUNTIL(rem-1)) | (tmpDest2 & MASK_BITSUNTIL(rem-1));
	}
}

size_t Bitmap::bitCount() const
{
	size_t result = 0;
	const_cast<Bitmap*>(this)->apply_operation1(
		[&result](uint64_t* ptr) { result += __popcnt64(*ptr); return true; }
		);
	return result;
}

size_t Bitmap::bitCount(size_t first, size_t last) const
{
	size_t result = 0;
	const_cast<Bitmap*>(this)->apply_operation1(
		[&result](uint64_t* ptr) { result += __popcnt64(*ptr); return true; },
		first,
		last
	);
	return result;
}


bool Bitmap::isZero() const
{
	bool result = true;
	const_cast<Bitmap*>(this)->apply_operation1(
		[&result](uint64_t* ptr) { if (*ptr != 0) { result = false; return false; } return true; }
	);
	return result;
}

BitmapBuf Bitmap::andNot(const Bitmap& other) const
{
	assert_eq(this->size, other.size);
	BitmapBuf result;
	result.resize(this->size);
	this->andNot(other, result);
	return result;
}

void Bitmap::andNot(const Bitmap& other, Bitmap& result) const
{
	this->apply_operation3(
		other, result,
		[&result](uint64_t* srcA, uint64_t* srcB, uint64_t* dest) { *dest = *srcA & ~*srcB; }
		);
}

BitmapBuf Bitmap::operator^(const Bitmap& other) const
{
	assert_eq(this->size, other.size);
	BitmapBuf result;
	result.resize(this->size);
	this->apply_operation3(
		other, result,
		[&result](uint64_t* srcA, uint64_t* srcB, uint64_t* dest) { *dest = *srcA ^ *srcB; }
	);
	return result;
}

#include <bitset>

void Bitmap::print()
{
	int64_t idx = 0;
	const_cast<Bitmap*>(this)->apply_operation1(
		[&idx](uint64_t* ptr) { idx++; std::cerr << std::bitset<64>(*ptr) << std::endl; return true; }
	);
}

void Bitmap::printNonZero()
{
	int64_t idx = 0;
	bool span = false;
	const_cast<Bitmap*>(this)->apply_operation1(
		[&idx, &span](uint64_t* ptr) {
			idx++;
			if (*ptr == 0) {
				if (span)
					std::cerr << std::endl;
				span = false;
				return true;
			}
			if (!span)
				std::cerr << std::to_string(idx) << ": ";
			span = true;
			std::cerr << std::bitset<64>(*ptr);
//			std::cerr << std::bitset<64>(*ptr) << std::endl;
			return true;
		}
	);
	if (span)
		std::cerr << std::endl;
}


void Bitmap::printBuf(void* buf, size_t size)
{
	Bitmap bmp{ buf, size };
	bmp.print();
}



BitmapBuf::BitmapBuf(size_t size) {
	this->resize(size);
}

BitmapBuf::BitmapBuf(BitmapBuf&& other)
{
	*this = std::move(other);
}

BitmapBuf& BitmapBuf::operator=(BitmapBuf&& other)
{
	this->data = other.data;
	other.data = nullptr;
	this->size = other.size;
	other.size = 0;
	this->buffer = std::move(other.buffer);
	return *this;
}

void BitmapBuf::resize(size_t size) {
	buffer.resize((size + 7) / 8);
	this->data = static_cast<uint64_t*>((void*)(buffer.data()));
	this->size = size;
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
	if (current.offset >= (LCN)size) {
		this->ptr = nullptr;
		return;
	}
	size_t bit_offset = current.offset % BLOCK_BITS;

	//Finish the open word
	uint64_t word = *ptr >> bit_offset;
	while (bit_offset < BLOCK_BITS - 1 && (size_t)current.offset < size-1) { //size-1 because we'll bit-test AFTER increment
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
	while (bit_offset < BLOCK_BITS - 1 && current.length < remainingSize-1) { //size-1 because we'll bit-test AFTER increment
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
	if (current.length >= remainingSize)
		return;

	//Now do the non-0 block
	word = *ptr;
	while (current.length < remainingSize && ((word & 1ULL) == 1)) {
		current.length++;
		word = word >> 1;
	}
	if (current.length >= remainingSize)
		return;
}


void ClusterPrinter::print(Bitmap& bitmap)
{
	if (outputFile.empty()) return;

	std::ofstream file_stream;
	std::ostream* out = nullptr;
	if (outputFile == "-")
		out = &std::cout;
	else {
		file_stream.open(outputFile);
		if (!file_stream.is_open())
			throwLastOsError();
		file_stream.seekp(std::ofstream::end, 0);
		out = &file_stream;
	}
	printClusters(*out, bitmap, this->clustersAsSpans, this->separator);
}

void printClusterSpan(std::ostream& out, LCN lcnFirst, LCN len, bool printClustersAsSpans, const std::string& separator)
{
	if (printClustersAsSpans)
		out << lcnFirst << ":" << len << separator;
	else {
		while (len > 0) {
			out << lcnFirst << separator;
			len--;
			lcnFirst++;
		}
	}
}

void printClusters(std::ostream& out, Bitmap& bitmap, bool printClustersAsSpans, const std::string& separator)
{
	for (auto& run : BitmapSpans(&bitmap))
		printClusterSpan(out, run.offset, run.length, printClustersAsSpans, separator);
	out << std::endl;
}

