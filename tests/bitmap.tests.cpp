#include "bitmap.h"
#include "catch.hpp"

TEST_CASE("Bitmap to spans", "[Bitmap]") {
	BitmapBuf bmp;
	bmp.resize(4096);

	std::vector<ClusterRun> src;
	src.emplace_back(5, 5);
	src.emplace_back(15, 5);
	src.emplace_back(64, 1);
	src.emplace_back(127, 65);

	for (auto& run : src)
		bmp.set(run.offset, run.offset + run.length - 1);
//	bmp.set(5, 10);
//	bmp.set(15, 20);
//	bmp.set(64, 65);
//	bmp.set(127, 192);

	std::vector<ClusterRun> decoded;
	for (auto& run : BitmapSpans(&bmp))
		decoded.push_back(run);

	CHECK(decoded.size() == src.size());
	for (size_t i = 0; i < decoded.size(); i++)
		CHECK(src[i] == decoded[i]);
}