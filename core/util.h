#pragma once
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include "ntfs.h"

/*
OS and COM error handling.
These might have better gone to osutil.h
*/
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


class ComError : public std::runtime_error {
protected:
	HRESULT m_errorCode = 0;
public:
	ComError(HRESULT errorCode, const std::string& message)
		: m_errorCode(errorCode), std::runtime_error(message)
	{}
	inline HRESULT errorCode() { return this->m_errorCode; }
};

inline void throwComError(HRESULT err) {
	throw ComError(err, std::string{ "HRESULT = " }+std::to_string(err));
}
inline void throwComError(HRESULT err, const std::string& message) {
	throw ComError(err, message + std::string{ "\nHRESULT = " }+std::to_string(err));
}
inline HRESULT comCheck(HRESULT hr, const char* context) {
	if (FAILED(hr)) throwComError(hr, context);
	return hr;
}
#define HRCHECK(...) comCheck(__VA_ARGS__, #__VA_ARGS__)


/*
Assertions.
*/
class AssertionFailure : public std::runtime_error {
public:
	using runtime_error::runtime_error;
};

template<typename First>
inline std::string parampack_join_strings(const std::string& sep, First first)
{
	return first;
}

template<typename First, typename... Args>
inline std::string parampack_join_strings(const std::string& sep, First first, Args&&... args)
{
	return first + sep + parampack_join_strings(sep, args...);
}

inline AssertionFailure format_assert(const std::string& text)
{
	return AssertionFailure(
		std::string{ "Assertion failed: " }
		+ text
	);
};

template<typename... Args>
inline AssertionFailure format_assert(const std::string& text, Args&&... args) {
	return AssertionFailure(
		std::string{ "Assertion failed: " }
		+ text + "\n"
		+ parampack_join_strings("\n", args...)
	);
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#undef assert
#define assert(COND,...) \
	do { \
		if(!(COND)) throw format_assert(std::string(__FILE__ "@" STR(__LINE__) ":\n" #COND), __VA_ARGS__); \
	} while (0)

#define assert_rel(VAL1,VAL2,REL,...) \
	do { \
		if(!(VAL1 REL VAL2)) throw format_assert(std::string(__FILE__ "@" STR(__LINE__) ":\n" #VAL1 " (") + std::to_string(VAL1) + ") " #REL " " #VAL2 " (" + std::to_string(VAL2) + ")", __VA_ARGS__); \
	} while (0)

#define assert_eq(VAL1,VAL2,...) assert_rel(VAL1,VAL2,==)
#define assert_neq(VAL1,VAL2,...) assert_rel(VAL1,VAL2,!=)
#define assert_gt(VAL1,VAL2,...) assert_rel(VAL1,VAL2,>)
#define assert_greq(VAL1,VAL2,...) assert_rel(VAL1,VAL2,>=)
#define assert_lt(VAL1,VAL2,...) assert_rel(VAL1,VAL2,<)
#define assert_leq(VAL1,VAL2,...) assert_rel(VAL1,VAL2,>=)




/*
Encoding conversions
*/
std::string wcharToUtf8(const std::wstring& input);
std::string wcharToUtf8(const wchar_t* first, const wchar_t* last);
std::wstring utf8ToWchar(const std::string& input);



/*
Logging
*/
enum Verbosity {
	Error = 0,
	Warning,
	Info,
	Verbose,
	Debug
};

class NullBuffer : public std::streambuf {
public:
	int overflow(int c) override {
		return std::char_traits<char>::not_eof(c);
	}
};

struct NullStream : public std::ostream {
public:
	NullStream() : std::ostream(&buffer) {}
	template <typename T>
	const NullStream& operator<<(const T&) const { return *this; }
private:
	NullBuffer buffer;
};

class LogPrinter {
public:
	static Verbosity verbosity;
	static NullStream g_nullStream;
	static bool humanReadableSizes;
};

//Steal Qt's names because everyone knows them
#define qDebug() (((int)LogPrinter::verbosity >= (int)Verbosity::Debug) ? std::cerr : LogPrinter::g_nullStream)
#define qVerbose() (((int)LogPrinter::verbosity >= (int)Verbosity::Verbose) ? std::cerr : LogPrinter::g_nullStream)
#define qInfo() (((int)LogPrinter::verbosity >= (int)Verbosity::Info) ? std::cerr : LogPrinter::g_nullStream)
#define qWarning() (((int)LogPrinter::verbosity >= (int)Verbosity::Warning) ? std::cerr : LogPrinter::g_nullStream) << "WARNING: "
#define qError() (((int)LogPrinter::verbosity >= (int)Verbosity::Error) ? std::cerr : LogPrinter::g_nullStream) << "ERROR: "


std::string dataSizeToStr(size_t sizeInBytes);

struct DataSizePrinter {
protected:
	DWORD BytesPerCluster = 0;
public:
	DataSizePrinter(DWORD BytesPerCluster) : BytesPerCluster(BytesPerCluster) {}
	std::string bytes(size_t count) { return dataSizeToStr(count); }
	std::string clusters(LCN count) { return std::to_string(count) + " (" + dataSizeToStr(count*BytesPerCluster) + ")"; }
};



/*
Operation time measurements
*/
struct PerformanceFrequency {
public:
	LARGE_INTEGER value;
	PerformanceFrequency() {
		if (!QueryPerformanceFrequency(&value))
			value.QuadPart = 1000; //dk any better
	}
	inline int64_t toMsec(int64_t val) {
		return val / (value.QuadPart / 1000);
	}
};

inline PerformanceFrequency& freq()
{
	static PerformanceFrequency p_freq;
	return p_freq;
}

struct MeasureTime {
protected:
	const char* name;
	LARGE_INTEGER m_start;
public:
	inline MeasureTime() : name("Tm") {
		QueryPerformanceCounter(&m_start);
	}
	inline MeasureTime(const char* name) : name(name) {
		QueryPerformanceCounter(&m_start);
	}
	//Auto-stop at scope exit
	inline ~MeasureTime() {
		this->done();
	}
	//Call to stop earlier than at scope exit
	inline void done() {
		if (m_start.QuadPart != 0) {
			print(name);
			m_start.QuadPart = 0;
		}
	}
	//Print at arbitrary intermediate points
	void print(const char* name) {
		LARGE_INTEGER tm;
		QueryPerformanceCounter(&tm);
		qVerbose() << name << ": " << freq().toMsec(tm.QuadPart - m_start.QuadPart) << "ms" << std::endl;
	}
};

//  Operation...
//  Operation took: 25ms
struct ScopedOp : public MeasureTime {
public:
	inline ScopedOp(const char* name)
		: MeasureTime(name)
	{
		qInfo() << name << "..." << std::endl;
	}
};



/*
Progress callbacks for various operations.
*/
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



/*
Filename printing.
*/
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

struct FilenamePrinter : public FilePrinter {
public:
	int BytesPerCluster = 1;
	void printOne(SegmentNumber segmentNo, const std::string& filename, LCN clusterCount);
};



/*
Cluster iteration and slicing.
*/
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

