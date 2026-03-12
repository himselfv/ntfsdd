#pragma once
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
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


#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define assert(COND) \
	do { \
    if(!(COND)) throw std::runtime_error( \
        std::string("Assertion failed: " __FILE__ "@" STR(__LINE__) ": " #COND) \
    ); \
	} while (0)


class ProgressCallback {
protected:
	uint64_t m_max = 0;
	uint64_t m_lastCall = 0;
	uint64_t m_onceEvery = 1;
	virtual void progress_int(uint64_t value);
public:
	virtual ~ProgressCallback();
	virtual void setMax(uint64_t value);
	void setOnceEvery(uint64_t value);
	inline void progress(uint64_t value, bool force) {
		if ((value >= this->m_lastCall + this->m_onceEvery) || force) {
			this->m_lastCall = value;
			this->progress_int(value);
		}
	}
};

class SimpleConsoleProgressCallback : public ProgressCallback {
protected:
	std::string m_operationName;
public:
	SimpleConsoleProgressCallback(std::string&& operationName);
	inline void setOperationName(const std::string& value) { this->m_operationName = value;  }
	virtual void progress_int(uint64_t value);
};

typedef SimpleConsoleProgressCallback ConsoleProgressCallback;



struct FilePrinter {
protected:
	std::ofstream m_fileStream {};
	std::ostream* out {};
public:
	std::string outputFile{};
	std::string separator = " ";
	inline bool active() { return !this->outputFile.empty(); }
	void open();
	void printOne(const std::string& entry);
};



struct ClusterRun {
	LCN offset = 0;
	LCN length = 0;
	ClusterRun() {}
	ClusterRun(LCN offset, LCN length) : offset(offset), length(length) {}
	bool operator==(const ClusterRun& other) const { return (offset == other.offset) && (length == other.length); }
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

