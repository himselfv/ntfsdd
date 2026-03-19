#pragma once
#include <cstdint>
#include "ntfs.h"
#include "util.h"

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


std::string readChars(wchar_t* first, size_t len);

inline std::string attrNameStr(ATTRIBUTE_RECORD_HEADER* attr) {
	return readChars((wchar_t*)((char*)attr + attr->NameOffset), attr->NameLength);
}

struct AttrFilename {
public:
	FILE_NAME* fn = nullptr;
	AttrFilename(ATTRIBUTE_RECORD_HEADER* attr);

	inline std::string name()
	{
		//Sic! wcharToUtf8 wants one char after the last one
		return wcharToUtf8(fn->FileName, fn->FileName + fn->FileNameLength);
	}

	inline operator std::string()
	{
		return this->name();
	}
};