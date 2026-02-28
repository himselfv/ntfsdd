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

	auto BytesPerCluster = this->src.volumeData().BytesPerCluster;
	assert(BytesPerCluster > 0);

	LCN lastProgress = 0;
	this->onProgress(0);

	//Max size of the single chunk for a read operation, in clusters
	//Bigger spans will be processed in chunks. Too large chunks would hinder parallelization when chunks of wildly different sizes are processed one after another.
	static constexpr LCN BATCH_LEN = 160;
	int64_t BATCH_SZ = BATCH_LEN * BytesPerCluster;

	AsyncFileReader srcReader(src.h(), 16, BATCH_SZ);
	AsyncFileReader destReader(dest.h(), 16, BATCH_SZ);

	auto slicedRuns = slice_runs(BitmapSpans(&srcDiff), BATCH_LEN);
	auto sliceIt = slicedRuns.begin();

	while (true) {
		//Часть 1. Запихиваем команды чтения в очередь
		while (sliceIt != slicedRuns.end()) {
			if (!srcReader.try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)))
				break;
			assert(destReader.try_push_back(sliceIt->offset*BytesPerCluster, (uint32_t)(sliceIt->length*BytesPerCluster)));
			++sliceIt;
		}

		//Часть 2. Достаём следующий элемент.
		//Проще за цикл вытаскивать по одному и добавлять по одному. Если сделано несколько, циклы подряд будут быстрыми.
		uint32_t bytesRead = 0, bytesRead2 = 0;
		uint64_t offset = 0, offset2 = 0;
		auto srcPtr = srcReader.finalize_front(&bytesRead, &offset);
		if (srcPtr == nullptr) {
			assert(sliceIt == slicedRuns.end());
			assert(srcReader.pending_count == 0);
			assert(destReader.pending_count == 0);
			break;
		}
		assert(offset % BytesPerCluster == 0);
		assert(bytesRead % BytesPerCluster == 0);
		auto destPtr = destReader.finalize_front(&bytesRead2, &offset2);
		assert(destPtr != nullptr);
		assert(bytesRead == bytesRead2);
		assert(offset == offset2);
		
		stats.runsChecked++;

		LCN lcn = offset / BytesPerCluster;
		LCN lastClean = lcn - 1;

		//Compare these cluster by cluster
		while (bytesRead > 0) {
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
			bytesRead -= BytesPerCluster;
			stats.clustersChecked++;
		}
		//We do not support "dirty spans" across chunk boundaries so finalize one if we have one
		if (lastClean != lcn - 1)
			this->onDirtySpan(lastClean + 1, lcn - lastClean - 1, srcPtr - (lcn - lastClean - 1)*src.volumeData().BytesPerCluster);

		srcReader.pop_front();
		destReader.pop_front();

		if (stats.clustersChecked - lastProgress > 50000) {
			lastProgress = stats.clustersChecked;
			this->onProgress((offset2 + bytesRead2) / BytesPerCluster);
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
