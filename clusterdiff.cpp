#pragma once
#include "clusterdiff.h"
#include "ntfsutil.h"
#include "ntfsmft.h"


ClusterDiffComparer::ClusterDiffComparer(Volume& src, Volume& dest)
	: src(src), dest(dest)
{
}

ClusterDiffComparer::~ClusterDiffComparer()
{
}


void ClusterDiffComparer::process(CandidateClusterMap& srcDiff)
{
	//Comparison unit length.
	//Must be a multiple of logical sector size. Read batches must be a multiple of this.

	//Ideally this is physical sector size, except SSDs lie and emulate 512b where they have 4096b, so in this case we want 4096.
	//TODO: Allow the user to override and give us True Physical Sector Size.
	//int64_t COMPARISON_UNIT_SZ = src.extendedVolumeData().BytesPerPhysicalSector;
	//Well, there's a safer way: work in clusters. It's not ideal if the true sector size is less, but it is also not bad. This is how the OS handles it after all.

	LCN lastProgress = 0;
	this->onProgress(0);

	//Max size of the single chunk for a read operation, in clusters
	//Bigger spans will be processed in chunks. Too large chunks would hinder parallelization when chunks of wildly different sizes are processed one after another.
	static constexpr LCN BATCH_LEN = 160;
	int64_t BATCH_SZ = BATCH_LEN * src.volumeData().BytesPerCluster;

	std::vector<uint8_t> srcBuf;
	srcBuf.resize(BATCH_SZ);
	std::vector<uint8_t> destBuf;
	destBuf.resize(BATCH_SZ);

	Overlapped srcOl;
	Overlapped destOl;

	HANDLE waitHandles[2] = { srcOl.hEvent, destOl.hEvent };

	for (auto& run : BitmapSpans(&srcDiff)) {
		stats.runsChecked++;

		LCN lcn = run.offset;
		int64_t offsetBytes = run.offset * src.volumeData().BytesPerCluster;
		srcOl.Offset = (DWORD)offsetBytes;
		srcOl.OffsetHigh = offsetBytes >> sizeof(srcOl.Offset) * 8;
		destOl.Offset = srcOl.Offset;
		destOl.OffsetHigh = srcOl.OffsetHigh;
		//Will be auto-incremented with reads

		LCN remainingLen = run.length;
		while (remainingLen > 0) {
			LCN len = remainingLen;
			if (len > BATCH_LEN)
				len = BATCH_LEN;
			remainingLen -= len;

			LCN lastClean = lcn - 1;

			int64_t bytesToRead = len * src.volumeData().BytesPerCluster;
			OSCHECKBOOL(src.read(srcBuf.data(), (DWORD)bytesToRead, nullptr, &srcOl));
			OSCHECKBOOL(dest.read(destBuf.data(), (DWORD)bytesToRead, nullptr, &destOl));

			auto res = WaitForMultipleObjects(2, waitHandles, TRUE, INFINITE);
			if (res < WAIT_OBJECT_0 || res > WAIT_OBJECT_0 + 1)
				throwLastOsError();

			DWORD bytesRead = 0;
			OSCHECKBOOL(src.getOverlappedResult(&srcOl, &bytesRead, TRUE));
			assert(bytesRead == bytesToRead);
			OSCHECKBOOL(src.getOverlappedResult(&destOl, &bytesRead, TRUE));
			assert(bytesRead == bytesToRead);

			//Compare these cluster by cluster
			auto srcPtr = srcBuf.data();
			auto destPtr = destBuf.data();
			while (len > 0) {
				auto diff = memcmp(srcPtr, destPtr, src.volumeData().BytesPerCluster);
				if (diff != 0) {
					stats.diffCount++;
					this->onDirty(lcn, srcPtr);
				}
				else {
					if (lastClean != lcn - 1)
						this->onDirtySpan(lastClean + 1, lcn - lastClean - 1, srcPtr - (lcn - lastClean - 1)*src.volumeData().BytesPerCluster);
					lastClean = lcn;
				}
				lcn++;
				len--;
				stats.clustersChecked++;
			}
			//We do not support "dirty spans" across chunk boundaries so finalize one if we have one
			if (lastClean != lcn - 1)
				this->onDirtySpan(lastClean + 1, lcn - lastClean - 1, srcPtr - (lcn - lastClean - 1)*src.volumeData().BytesPerCluster);
		}

		if (stats.clustersChecked - lastProgress > 50000) {
			lastProgress = stats.clustersChecked;
			this->onProgress(run.offset + run.length);
		}
	}
}

void ClusterDiffComparer::onProgress(LCN lcn)
{
	if (lcn == 0) {
		this->m_progress_prevStats = this->stats;
		this->m_progress_tm = GetTickCount();
		return;
	}

	int64_t thisClusterCount = this->stats.clustersChecked - this->m_progress_prevStats.clustersChecked;
	int64_t thisRunCount = this->stats.runsChecked - this->m_progress_prevStats.runsChecked;
	auto t2 = GetTickCount() - this->m_progress_tm + 1; //To avoid div by zero.
	this->m_progress_tm += t2;

	std::cout << "Clusters: " << stats.clustersChecked << ", runs: " << thisRunCount << ", t=" << t2 << ", cpm=" << (double)thisClusterCount / t2 << ", rpm=" << (double)thisRunCount / t2 << std::endl;
	this->m_progress_prevStats = this->stats;
}

void ClusterDiffComparer::onDirty(LCN lcn, void* data)
{
	//std::cout << "Diff: " << lcn << std::endl;
	//TODO: Add to write queue
}

void ClusterDiffComparer::onDirtySpan(LCN lcnFirst, LCN len, void* data)
{
	//std::cout << "Span: " << lcnFirst << ":" << len << std::endl;
}
