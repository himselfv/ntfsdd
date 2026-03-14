#include "bitmap.h"
#include "catch.hpp"

inline int rand_int(int min, int max)
{
	double pt = (double)rand() / RAND_MAX;
	return min + (int)(pt * max);
}

TEST_CASE("Simple set/clear", "[Bitmap]") {
	BitmapBuf bmp;
	bmp.resize(64);
	CHECK(bmp.size == 64);

	//Tests in the first 64 bits

	CHECK(*bmp.data == 0);
	for (auto i = 0; i < 64; i++) {
		bmp.set(i, i);
		CHECK(*bmp.data == (1ULL << i));
		bmp.clear(i, i);
		CHECK(*bmp.data == 0);
	}
	for (auto i = 0; i < 64-1; i++) {
		bmp.set(i, i + 1);
		CHECK(*bmp.data == ((1ULL << i) | (1ULL << (i + 1))));
		bmp.clear(i, i);
		CHECK(*bmp.data == (1ULL << (i+1)));
		bmp.clear(i, i+1);
		CHECK(*bmp.data == 0);
	}
}

TEST_CASE("Cross-border set/clear", "[Bitmap]") {
	BitmapBuf bmp;
	bmp.resize(128);
	CHECK(bmp.size == 128);

	//Tests in the first 64 bits

	CHECK(bmp.data[0] == 0);
	CHECK(bmp.data[1] == 0);

	bmp.set(63, 65);
	CHECK(bmp.data[0] == 1ULL << 63);
	CHECK(bmp.data[1] == ((1ULL << 0) | (1ULL << 1)));

	bmp.clear(63, 63);
	CHECK(bmp.data[0] == 0);
	CHECK(bmp.data[1] == ((1ULL << 0) | (1ULL << 1)));

	bmp.clear(64, 64);
	CHECK(bmp.data[1] == (1ULL << 1));
}


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

TEST_CASE("Bitmap mass ops", "[Bitmap]") {
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

	bmp1.set(1020, 1280);

	CHECK(bmp1.bitCount(0, 5) == 0);
	CHECK(bmp1.bitCount(6, 12) == 5);
	CHECK(bmp1.bitCount(62, 1000) == 2);
	CHECK(bmp1.bitCount(1010, 1020) == 1);
	CHECK(bmp1.bitCount(1020, 1030) == 11);
}

TEST_CASE("Random set/verify", "[Bitmap]") {
	BitmapBuf bmp1;
	bmp1.resize(4096);
	bmp1.clear_all();


	for (int i = 0; i < 1000; i++) {
		ClusterRun run;
		//run.offset = rand_int(0, bmp1.size);
		run.offset = 0;
		run.length = 64;//rand_int(1, 64);

		CAPTURE(run.offset, run.length);
		CHECK(bmp1.bitCount(run.offset, run.offset + run.length - 1) == 0);
		bmp1.set(run.offset, run.offset + run.length - 1);
		CHECK(bmp1.bitCount(run.offset, run.offset + run.length - 1) == run.length);
		bmp1.clear(run.offset, run.offset + run.length - 1);
		CHECK(bmp1.bitCount(run.offset, run.offset + run.length - 1) == 0);
	}
}


TEST_CASE("Compare", "[Bitmap]") {
	BitmapBuf bmp1;
	bmp1.resize(4096);
	bmp1.clear_all();

	CHECK(0 == Bitmap::memcmp(bmp1.data, bmp1.data, bmp1.size, 0, 0));
	CHECK(0 == Bitmap::memcmp(bmp1.data, bmp1.data, bmp1.size-64, 64, 64));

	BitmapBuf bmp2;
	bmp2.resize(bmp1.size);
	bmp2.clear_all();

	CHECK(0 == Bitmap::memcmp(bmp1.data, bmp2.data, bmp1.size, 0, 0));

	bmp2.set(1025, 1025);
	CHECK(0 != Bitmap::memcmp(bmp1.data, bmp2.data, bmp1.size, 0, 0));
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
