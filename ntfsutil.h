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
	LCN offset;
	LCN length;
};

class DataRunIterator {
public:
	struct Iterator {
		uint8_t* ptr = nullptr;
		ClusterRun run{ 0, 0 };

		Iterator(uint8_t* ptr) : ptr(ptr) { this->readCurrent(); }

		// Access the current value
		ClusterRun& operator*() { return run; }

		// Comparison for the loop termination
		bool operator!=(const Iterator& other) const {
			return ptr != other.ptr;
		}

		inline void readCurrent()
		{
			if (ptr == nullptr) return;
			if (*ptr == 0x00) { ptr = nullptr; return; } //To satisfy comparison with end().
			auto sz = *ptr;
			ptr++;
			run.length = ReadUnsignedValue(ptr, sz & 0x0F);
			ptr += sz & 0x0F;
			run.offset += ReadSignedValue(ptr, sz >> 4);
			ptr += (sz >> 4);
		}

		// Advance the generator
		Iterator& operator++() {
			readCurrent();
			return *this;
		}
	};

	ATTRIBUTE_RECORD_HEADER* attr = nullptr;

	DataRunIterator(ATTRIBUTE_RECORD_HEADER* attr) : attr(attr) {}

	Iterator begin() { return{ (uint8_t*)attr + attr->Form.Nonresident.MappingPairsOffset }; }
	Iterator end() { return{ nullptr }; }
};

