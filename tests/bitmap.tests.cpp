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

TEST_CASE("Bitmap ops", "[Bitmap]") {
	BitmapBuf bmp1;
	bmp1.resize(4096);
	bmp1.clear_all();

	CHECK(bmp1.isZero());

	BitmapBuf bmp2;
	bmp2.resize(4096);
	bmp2.clear_all();

	CHECK(bmp2.isZero());

	bmp1.set(8, 15);
	bmp2.set(16, 31);
	bmp1.set(32, 63);
	bmp2.set(32, 63);

	CHECK(!bmp1.isZero());
	CHECK(!bmp2.isZero());
	CHECK(bmp1.bitCount() == 40);
	CHECK(bmp2.bitCount() == 48);

	auto and_not = bmp1.andNot(bmp2);
	auto xor = bmp1 ^ bmp2;

	CHECK(and_not.get(9));
	CHECK(!and_not.get(17));
	CHECK(!and_not.get(33));

	CHECK(and_not.bitCount()==8);

	CHECK(xor.get(9));
	CHECK(xor.get(17));
	CHECK(!xor.get(33));
	CHECK(xor.bitCount() == 24);
}

TEST_CASE("Bitmap span all 1s", "[Bitmap]") {
	BitmapBuf bmp;
	bmp.resize(4096);
	bmp.set(0, 4095);

	std::vector<ClusterRun> decoded;
	for (auto& run : BitmapSpans(&bmp))
		decoded.push_back(run);

	CHECK(decoded.size() == 1);
	CHECK(decoded[0].offset == 0);
	CHECK(decoded[0].length == 4096);

	bmp.resize(127);
	bmp.set(0, 79);
	bmp.clear(80, 126);
	decoded.clear();
	for (auto& run : BitmapSpans(&bmp))
		decoded.push_back(run);
	CHECK(decoded.size() == 1);
	CHECK(decoded[0].offset == 0);
	CHECK(decoded[0].length == 80);
}
