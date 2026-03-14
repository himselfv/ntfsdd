#pragma once
#include <vector>
#include "util.h"

struct BitmapBuf;

//Bitmap access to an externally-managed data.
struct Bitmap {
public:
	constexpr static size_t BLOCK_BITS = sizeof(uint64_t) * 8;

	uint64_t* data = nullptr;
	size_t size = 0; //Size in bits
	Bitmap() {};
	Bitmap(void* data, size_t size) : data(static_cast<uint64_t*>(data)), size(size) {};
	inline bool get(size_t idx) const { return 0 != (((uint8_t*)data)[idx / 8] & (1 << (idx % 8))); }
	void set(size_t lo, size_t hi);
	inline void set(const ClusterRun& run) { this->set(run.offset, run.offset + run.length - 1);  }
	void clear(size_t lo, size_t hi);
	void clear_all();

public:
	static int64_t memcmp(const void* bitmap1, const void* bitmap2, size_t bitcnt, size_t offset1 = 0, size_t offset2 = 0);

private:
	template <typename Op64>
	void apply_operation1(Op64 op64);
	template <typename Op64>
	void apply_operation1(Op64 op64, size_t first, size_t last);
	template <typename Op64>
	void apply_operation3(const Bitmap& other, Bitmap& result, Op64 op64) const;

public:
	size_t bitCount() const;
	size_t bitCount(size_t first, size_t last) const;
	bool isZero() const;
	BitmapBuf andNot(const Bitmap& other) const;
	void andNot(const Bitmap& other, Bitmap& result) const;
	BitmapBuf operator^(const Bitmap& other) const;

	void print();
	void printNonZero();
	static void printBuf(void* buf, size_t size);
};

//Automatically managed buffer with bitmap access.
struct BitmapBuf : public Bitmap {
public:
	std::vector<uint8_t> buffer;
	BitmapBuf() {}
	BitmapBuf(size_t size);
	BitmapBuf(BitmapBuf&& other);
	BitmapBuf& operator=(BitmapBuf&& other);
	void resize(size_t size);
};

class BitmapSpans {
public:
	static constexpr size_t BLOCK_BITS = Bitmap::BLOCK_BITS;
	struct Iterator {
		//Normally always points at 0 or at the end
		const uint64_t* ptr;
		size_t size = 0;

		ClusterRun current;

		Iterator(const uint64_t* ptr, size_t size) : ptr(ptr), size(size) { current.offset = 0; }

		// Comparison for the loop termination
		bool operator!=(const Iterator& other) const {
			return ptr != other.ptr;
		}

		// Access the current value
		inline ClusterRun operator*() { return current; }

		void findFirst();

		//On entrance, must point to 0.
		//Skips the 0s. On exit, either points to 1 or nulls the ptr.
		void skip0s();

		//On entrance, must point to 1.
		//Reads the 1s until hitting either 0 or EOF. Does not null the ptr, allowing this to be a hit. Next skip0s() is going to do that.
		void eat1s();

		// Advance the generator
		Iterator& operator++() {
			this->skip0s();
			if (this->ptr)
				this->eat1s();
			return *this;
		}
	};


	const uint64_t* ptr;
	size_t size = 0;
	BitmapSpans(Bitmap* bmp) : ptr(bmp->data), size(bmp->size) {}
	BitmapSpans(const uint64_t* ptr, size_t size) : ptr(ptr), size(size) {}

	inline Iterator begin() {
		// We must ensure we start at the first '1' bit
		Iterator it(ptr, size);
		it.findFirst();
		return it;

	}
	Iterator end() { return{ nullptr, 0 }; }
};


struct ClusterPrinter {
public:
	std::string outputFile{};
	bool clustersAsSpans = false;
	std::string separator = " ";
	void print(Bitmap& bitmap);
};

void printClusterSpan(std::ostream& out, LCN lcnFirst, LCN len, bool printClustersAsSpans, const std::string& separator);
void printClusters(std::ostream& out, Bitmap& bitmap, bool printClustersAsSpans, const std::string& separator);
