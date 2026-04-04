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




void bmpEncodeDecode(BitmapBuf& bmp, const std::vector<ClusterRun>& src)
{
	bmp.clear_all();

	for (auto& run : src)
		bmp.set(run.offset, run.offset + run.length - 1);

	std::vector<ClusterRun> decoded;
	for (auto& run : BitmapSpans(&bmp))
		decoded.push_back(run);

	CHECK(decoded.size() == src.size());
	for (size_t i = 0; i < decoded.size(); i++)
		CHECK(src[i] == decoded[i]);
}

TEST_CASE("Bitmap to spans", "[Bitmap]\\[Spans]") {
	BitmapBuf bmp;
	bmp.resize(4096);

	//Empty bitmap should have no spans
	for (auto& run : BitmapSpans(&bmp))
		CHECK(false);

	//Span at the start
	bmp.clear_all();
	bmp.set(0, 1000);
	for (auto& run : BitmapSpans(&bmp)) {
		CHECK(run.offset == 0);
		CHECK(run.length == 1001);
	}

	//Span in the middle
	bmp.clear_all();
	bmp.set(1000, 2000);
	for (auto& run : BitmapSpans(&bmp)) {
		CHECK(run.offset == 1000);
		CHECK(run.length == 1001);
	}

	//Span at the end
	bmp.clear_all();
	bmp.set(3095, 4095);
	for (auto& run : BitmapSpans(&bmp)) {
		CHECK(run.offset == 3095);
		CHECK(run.length == 1001);
	}


	//From here on, we will add many test cast spans at once,
	//decode them, reencode them and verify that the resulting set matchines
	std::vector<ClusterRun> src;

	//A bunch of random spans to warm up
	src.emplace_back(5, 5);
	src.emplace_back(15, 5);
	src.emplace_back(64, 1);
	src.emplace_back(127, 65);
	bmpEncodeDecode(bmp, src);


	//The main set
	//Blocks are 64 bit
	//Tests are spaced 128 bits apart so that there's an empty block between each two
	src.clear();

	//Single-block spans
	src.emplace_back(256, 28);	//256: Beginning of the block
	src.emplace_back(419, 28);	//384: End of the block
	src.emplace_back(524, 28);	//512: Middle of the block
	src.emplace_back(628, 28);	//640: Cross the block border

	src.emplace_back(768, 1);	//768: Single bit at the beginning
	src.emplace_back(959, 1);	//896: Single bit at the end
	src.emplace_back(1035, 1);	//1024: Single bit in the middle

	src.emplace_back(1151, 66);	//1152: Bridge one full block (. ..... .)

	bmpEncodeDecode(bmp, src);


	//Some more special cases on the buffer borders
	
	//start_10 and end_01
	src.clear();
	src.emplace_back(0, 1);
	src.emplace_back(4095, 1);
	bmpEncodeDecode(bmp, src);

	//start_010 and end_010
	src.clear();
	src.emplace_back(1, 1);
	src.emplace_back(4094, 1);
	bmpEncodeDecode(bmp, src);

	//Same, but the final bits are not on the block boundary
	bmp.resize(4107);

	src.clear();
	src.emplace_back(4106, 1);
	bmpEncodeDecode(bmp, src);

	src.clear();
	src.emplace_back(4105, 1);
	bmpEncodeDecode(bmp, src);

	//Failing case from the app:
	//Final span ends on ONE BIT LESS than a block. Accidentally, the remaining bit is also set.
	bmp.resize(255);
	bmp.buffer[23] = 0xFF; //Needs at least some prefix before the full block
	*((uint64_t*)&bmp.buffer[24]) = ~0ULL; //Set all bits, even the remaining one at the end
	for (auto& run : BitmapSpans(&bmp)) {
		CHECK(run.offset == 184);
		CHECK(run.length == 71);
	}
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
