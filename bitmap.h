#pragma once
#include <vector>
#include "ntfsutil.h"

//Bitmap access to an externally-managed data.
struct Bitmap {
public:
	constexpr static size_t BLOCK_BITS = sizeof(uint64_t) * 8;
	uint64_t* data = nullptr;
	size_t size = 0; //Size in bits
	Bitmap() {};
	Bitmap(void* data) : data(static_cast<uint64_t*>(data)) {};
	inline bool get(size_t idx) { return 0 != (((uint8_t*)data)[idx / 8] & (1 << (idx % 8))); }
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


	Bitmap* bmp = nullptr;
	BitmapSpans(Bitmap* bmp) : bmp(bmp) {}

	inline Iterator begin() {
		// We must ensure we start at the first '1' bit
		Iterator it(bmp->data, bmp->size);
		it.findFirst();
		return it;

	}
	Iterator end() { return{ nullptr, 0 }; }
};

