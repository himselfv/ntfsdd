#pragma once
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include "ntfs.h"

//This might have better gone to osutil.h
class OsError : public std::runtime_error {
protected:
	DWORD m_errorCode = 0;
public:
	OsError(DWORD errorCode, const std::string& message)
		: m_errorCode(errorCode), std::runtime_error(message)
	{}
	inline DWORD errorCode() { return this->m_errorCode; }
};

inline void throwOsError(DWORD err) {
	throw OsError(err, std::string{ "Error " }+std::to_string(err));
}
inline void throwOsError(DWORD err, const std::string& message) {
	throw OsError(err, message + std::string{ "\nError " }+std::to_string(err));
}
inline void throwLastOsError() {
	throwOsError(GetLastError());
}
inline void throwLastOsError(const std::string& message) {
	throwOsError(GetLastError(), message);
}
#define OSCHECKBOOL(...) if(!__VA_ARGS__) throwLastOsError(#__VA_ARGS__);

#define assert(COND) if(!(COND)) throw std::runtime_error(#COND);



class AttributeIterator {
public:
	struct Iterator {
		ATTRIBUTE_RECORD_HEADER* attr = nullptr;

		Iterator(ATTRIBUTE_RECORD_HEADER* attr) : attr(attr) { this->readCurrent(); }

		// Access the current value
		ATTRIBUTE_RECORD_HEADER& operator*() { return *attr; }

		// Comparison for the loop termination
		bool operator!=(const Iterator& other) const {
			return attr != other.attr;
		}

		void readCurrent()
		{
			if (attr == nullptr) return;
			if (attr->TypeCode == 0xFFFFFFFF) { attr = nullptr; } //To satisfy comparison with end().
		}

		// Advance the generator
		Iterator& operator++() {
			attr = (ATTRIBUTE_RECORD_HEADER*)((char*)attr + attr->RecordLength);
			this->readCurrent();
			return *this;
		}
	};

	FILE_RECORD_SEGMENT_HEADER* header = nullptr;

	AttributeIterator(FILE_RECORD_SEGMENT_HEADER* header) : header(header) {}

	Iterator begin() { return{ (ATTRIBUTE_RECORD_HEADER*)((char*)header + header->FirstAttributeOffset) }; }
	Iterator end() { return{ nullptr }; }

	static ATTRIBUTE_RECORD_HEADER* findFirstAttr(FILE_RECORD_SEGMENT_HEADER* segment, ATTRIBUTE_TYPE_CODE typeCode) {
		for (auto& attr : AttributeIterator(segment)) {
			if (attr.TypeCode == typeCode)
				return &attr;
		}
		return nullptr;
	}
};


/*
LCN runs in nonresident attributes are variable-sized, from 1 to 15 bytes in length (in practice 8 bytes should be more than enough).
They have to be sign-extended: the target variable (8 bytes) has to inherit sign of the lower-byte (1-3-5 byte) source.
*/
int64_t ReadSignedValue(const uint8_t* buffer, size_t size);
int64_t ReadUnsignedValue(const uint8_t* buffer, size_t size);

struct ClusterRun {
	LCN offset = 0;
	LCN length = 0;
	ClusterRun() {}
	ClusterRun(LCN offset, LCN length) : offset(offset), length(length) {}
	bool operator==(const ClusterRun& other) const { return (offset == other.offset) && (length == other.length); }
};


#define DRI_SKIP_SPARSE	(1UL << 0)

class DataRunIterator {
public:
	struct Iterator {
		uint8_t* ptr = nullptr;
		ClusterRun run{ 0, 0 };
		uint32_t flags = 0;

		Iterator(uint8_t* ptr, uint32_t flags = 0) : ptr(ptr), flags(flags) {
			this->readCurrent();
		}

		// Access the current value
		ClusterRun& operator*() { return run; }

		// Comparison for the loop termination
		bool operator!=(const Iterator& other) const {
			return ptr != other.ptr;
		}

		static constexpr uint64_t LCN_SIGN_BIT = (1ULL << (sizeof(LCN) * 8 - 1));

		inline void readCurrent()
		{
			while (true) {
				if (ptr == nullptr) return;
				if (*ptr == 0x00) { ptr = nullptr; return; } //To satisfy comparison with end().
				auto sz = *ptr;
				ptr++;
				run.length = ReadUnsignedValue(ptr, sz & 0x0F);
				ptr += sz & 0x0F;

				sz >>= 4;
				// Special case: If the size of the offset value is zero, this indicates a "sparse part"
				// (zero block of a given length not stored anywhere on the disk).
				// Indicate this by sign bit in an offset. (Not zero! Zero is a valid offset).
				if (sz == 0)
					run.offset |= LCN_SIGN_BIT;
				else {
					//De-sparsify offset if the previous run had been sparse
					run.offset &= ~LCN_SIGN_BIT;
					run.offset += ReadSignedValue(ptr, sz);
				}
				ptr += sz;

				if ((run.offset < 0) && (flags & DRI_SKIP_SPARSE))
					continue;
				break;
			}
		}

		// Advance the generator
		Iterator& operator++() {
			readCurrent();
			return *this;
		}
	};

	ATTRIBUTE_RECORD_HEADER* attr = nullptr;
	uint32_t flags = 0;

	DataRunIterator(ATTRIBUTE_RECORD_HEADER* attr, uint32_t flags = 0) : attr(attr), flags(flags) {}

	Iterator begin() { return{ (uint8_t*)attr + attr->Form.Nonresident.MappingPairsOffset, flags }; }
	Iterator end() { return{ nullptr, 0 }; }
};



/*
Iterates over cluster runs in continuous blocks of no more than max_block_size clusters.
Usage:
	for (auto& run : slice_runs(MyBaseRunIterator(..)), max_block_len) { .. }
*/
template <typename BaseIterator>
class SlicedRunIterator {
public:
	using iterator_category = std::input_iterator_tag;
	using value_type = ClusterRun;
	using difference_type = std::ptrdiff_t;
	using pointer = ClusterRun*;
	using reference = ClusterRun&;

	SlicedRunIterator(BaseIterator it, BaseIterator end, size_t max_block_size)
		: it(it), end(end), max_size(max_block_size) {
		if (it != end) {
			current_run = *it;
			consume_chunk();
		}
	}

	// End iterator constructor
	SlicedRunIterator(BaseIterator end)
		: it(end), end(end), max_size(0), active(false) {}

	inline ClusterRun operator*() const { return current_chunk; }
	inline ClusterRun* operator->() { return &current_chunk; }

	SlicedRunIterator& operator++() {
		if (current_run.length > 0) {
			consume_chunk();
		}
		else {
			++it;
			if (it != end) {
				current_run = *it;
				consume_chunk();
			}
			else {
				active = false;
			}
		}
		return *this;
	}

	bool operator!=(const SlicedRunIterator& other) const {
		// If both are at the end
		if (!active && !other.active) return false;
		// Otherwise check underlying iterator and remaining length
		return it != other.it ||
			current_run.offset != other.current_run.offset ||
			active != other.active;
	}
	inline bool operator==(const SlicedRunIterator& other) const {
		return !(*this != other);
	}

protected:
	void consume_chunk() {
		active = true;

		current_chunk.offset = current_run.offset;
		current_chunk.length = max_size;
		if (current_chunk.length > current_run.length)
			current_chunk.length = current_run.length;

		current_run.offset += current_chunk.length;
		current_run.length -= current_chunk.length;
	}

	BaseIterator it;
	BaseIterator end;
	size_t max_size;
	ClusterRun current_run{ 0, 0 };   // The remainder of the current source run
	ClusterRun current_chunk{ 0, 0 }; // The current capped piece
	bool active = false;
};

template <typename BaseRange>
class SlicedRunRange {
	//Stores a reference in case we're passed a permanent base range,
	//or the instance itself in case of a temporary.
	BaseRange container_;
	size_t max_sz;
public:
	typedef typename decltype(std::declval<BaseRange>().begin()) BaseIterator;

	// We use forwarding to decide whether to store a reference or a copy
	SlicedRunRange(BaseRange&& container, size_t sz)
		: container_(std::forward<BaseRange>(container)), max_sz(sz) {}

	auto begin() -> SlicedRunIterator<BaseIterator> {
		return{ std::begin(container_), std::end(container_), max_sz };
	}
	auto end() -> SlicedRunIterator<BaseIterator> {
		return{ std::end(container_) };
	}
};
template <typename BaseRange>
SlicedRunRange<BaseRange> slice_runs(BaseRange&& container, size_t max_sz) {
	return SlicedRunRange<BaseRange>(std::forward<BaseRange>(container), max_sz);
}
