#include "catch.hpp"
#include "bitmap.h"
#include "util.h"

int rand_int(int min, int max)
{
	double pt = (double)rand() / RAND_MAX;
	return min + (int)(pt * max);
}

void randomFill(Bitmap& bmp)
{
	for (size_t i = 0; i < 100; i++) {
		int len = rand_int(0, (int)(bmp.size / 100));
		int start = rand_int(0, (int)(bmp.size - len - 1));
		bmp.set(ClusterRun{ start, len });
	}
}

TEST_CASE("SlicedRunEquivalence", "[ClusterRun]") {
	BitmapBuf bmp;
	bmp.resize(4096);

	BitmapBuf result1;
	BitmapBuf result2;
	result1.resize(bmp.size);
	result2.resize(bmp.size);

	for (size_t i = 0; i < 100; i++) {
		randomFill(bmp);
		result1.clear_all();
		result2.clear_all();
		int slice_size = i+1; //Just try all various slice sizes

		for (auto& span : BitmapSpans(&bmp))
			result1.set(span);

		for (auto& span : slice_runs(BitmapSpans(&bmp), slice_size))
			result2.set(span);

		CHECK(0 == Bitmap::memcmp(result1.data, result2.data, result1.size));
	}
}
