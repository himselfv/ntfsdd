#pragma once
#include "mftdir.h"


AttrFilename::AttrFilename(ATTRIBUTE_RECORD_HEADER* attr)
{
	assert(attr->FormCode != NONRESIDENT_FORM);
	fn = (FILE_NAME*)((char*)attr + attr->Form.Resident.ValueOffset);
}


void FilenameEntry::update(ATTRIBUTE_RECORD_HEADER& attr)
{
	assert(attr.FormCode != NONRESIDENT_FORM);
	FILE_NAME* fndata = (FILE_NAME*)((char*)&attr + attr.Form.Resident.ValueOffset);
	if (fndata->Flags & FILE_NAME_NTFS || !this->filenameNtfs) {
		this->filename = wcharToUtf8(fndata->FileName, fndata->FileName + fndata->FileNameLength);
		if (fndata->Flags & FILE_NAME_NTFS) this->filenameNtfs = true;
	}
	if (this->parentDir == -1 && fndata->ParentDirectory.mergedValue != 0)
		this->parentDir = fndata->ParentDirectory.segmentNumber();
}

void FilenameMap::process(SegmentNumber segmentNo, ATTRIBUTE_RECORD_HEADER& attr) {
	auto& entry = (*this)[segmentNo];
	entry.update(attr);
}

std::string FilenameMap::getFullPath(SegmentNumber segmentNo)
{
	std::string result{};
	while (segmentNo >= 0) {
		auto& entry = (*this)[segmentNo];
		std::string filename{};
		if (entry.filename.empty())
			filename = std::string{ "#" } +std::to_string(segmentNo);
		else
			filename = entry.filename;
		result = filename + (!result.empty() ? std::string{ "\\" } +result : std::string{});
		if (entry.parentDir == segmentNo) //Root dir does this
			segmentNo = -1;
		else
			segmentNo = entry.parentDir;
	}
	return result;
}



BitmapProcessor::BitmapProcessor(Volume* vol, BitmapBuf& bitmap)
	: AttributeCollectorProcessor(vol), m_bitmap(bitmap)
{
}

size_t BitmapProcessor::tryReadEntry(byte* buf, size_t len)
{
	if (!this->haveDataHeader()) return 0;
	if (this->m_bitmap.size == 0)
		this->m_bitmap.resize(this->sizeInBytes() * 8);
	assert(bytesProcessed + len <= this->m_bitmap.size / 8);
	memcpy((byte*)(this->m_bitmap.data) + bytesProcessed, buf, len);
	bytesProcessed += len;
	return len;
}



DirIndexProcessor::DirIndexProcessor(Volume* vol)
	: AttributeCollectorProcessor(vol), m_bitmapLoader(vol, m_bitmap)
{
	this->m_root.BytesPerIndexBuffer = 0; //Not initialized flag
}

//Pass index root here. It's always resident, only one instance of it in the multi-segment file.
void DirIndexProcessor::processIndexRoot(void* data, size_t len)
{
	assert(!this->haveIndexRoot(), "Second $INDEX_ROOT encountered in DirIndexProcessor!");
	assert(len >= sizeof(INDEX_ROOT));
	auto header = (INDEX_ROOT*)data;

	this->m_root = *header;
	assert(sizeof(INDEX_ALLOCATION_BUFFER) <= this->m_root.BytesPerIndexBuffer);
	assert(this->m_root.BytesPerIndexBuffer % vol->volumeData().BytesPerSector == 0);

	this->readIndexEntries(&header->IndexHeader);
}

void DirIndexProcessor::processBitmapAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	if (attr.FormCode == RESIDENT_FORM)
		this->m_bitmapLoader.processResidentAttr(attr);
	else
		this->m_bitmapLoader.addAttrChunk(&attr);
}


size_t DirIndexProcessor::tryReadEntry(byte* buf, size_t len)
{
	//We need $INDEX_ROOT to know the sizes and $BITMAP to know which Index Allocation Buffers are in use.
	if (!this->haveIndexRoot()) return 0;

	//$BITMAP is dynamically loaded. Check that we have the neccessary number of bits.
	if (this->m_bitmapLoader.bytesProcessed < (this->m_blockNo / 8) + 1)
		return 0;

	//Since we're here, we have chunk 0s of INDEX_ALLOCATION and BITMAP, so we know their sizes.
	//Let's do some asserts. Would be better to do them once on those initial chunk reads, but we have what we have.

	//Assert that the total size of INDEX_ALLOCATION is in multiples of BytesPerIndexBuffer,
	//and that the number of IndexBuffers is less than the bit count in Bitmap.
	//  "Less than" because 1. Bitmap rounds up to the byte (duh), 2. Reportedly, NTFS is lazy with $Bitmap shrinking.
	assert(this->sizeInBytes() % this->m_root.BytesPerIndexBuffer == 0);
	assert(this->sizeInBytes() / this->m_root.BytesPerIndexBuffer <= (this->m_bitmapLoader.sizeInBytes() * 8));

	//If the bit is clear, simply skip this allocation buffer
	if (!this->m_bitmap.get(this->m_blockNo)) {
		this->m_blockNo++;
		return this->m_root.BytesPerIndexBuffer;
	}

	/*
	INDEX_ALLOCATION is stored in Index Allocation Buffers (Blocks).
	Each block contains fixups which have to be applied.
	Let's just wait for the whole block to become available so as not to code a complex state machine.
	*/
	if (len < this->m_root.BytesPerIndexBuffer)
		return 0;

	auto header = (INDEX_ALLOCATION_BUFFER*)buf;
	sectorsCheckSignature(header->MultiSectorHeader, SIGNATURE_INDX);

	auto BytesPerSector = vol->volumeData().BytesPerSector;
	sectorsApplyFixups(&header->MultiSectorHeader, this->m_root.BytesPerIndexBuffer / BytesPerSector, BytesPerSector);

	this->readIndexEntries(&header->IndexHeader);

	this->m_blockNo++;
	return this->m_root.BytesPerIndexBuffer;
}


/*
Both INDEX_ROOT and each INDEX_ALLOCATION_BUFFER are followed by a number of INDEX_ENTRYs.
Thankfully both end with the same INDEX_HEADER describing the contents so we can process them uniformly.

Atm we eat data in whole INDEX_ALLOCATION_BUFFER blocks, so no "size_t tryRead" style at this level.
Maybe that's good because this system is designed for selective reading of whole INDEX_ALLOCATION_BUFFERs,
not for streaming that we do now.
Let's remain compatible.
*/
void DirIndexProcessor::readIndexEntries(INDEX_HEADER* header)
{
	//NTFS groups intermediate (non-leaf) entries into non-leaf blocks.
	//Since we're not interested in those, skip the whole block.
	if ((header->Flags & INDEX_NODE) != 0)
		return;

	auto data = (byte*)header + header->FirstIndexEntry;
	assert(header->FirstFreeByte > header->FirstIndexEntry);
	auto len = header->FirstFreeByte - header->FirstIndexEntry;
	
	//I know I just said we're not doing tryRead here, but...
	while (auto readSz = tryReadIndexEntry((byte*)data, len)) {
		data += readSz;
		len -= (ULONG)readSz;
	}

	/*
	FirstFreeByte is reportedly quad-word aligned so shouldn't we allow up to 15 bytes of slack?
	But when we process existing entries we simply append their Length; there's no other way.
	Yet we are supposedly jumping to quad-word-aligned start of the next INDEX_ENTRY.
	This means that the previous entry's Length itself should be quad-word aligned!
	If this is so, then it makes no sense to keep only the final Length unaligned and leave alignment slack space.
	Therefore I strongly suspect all Lengths are quad-aligned and alignment slack should be 0.
	*/
	assert_eq(len, 0, "Unprocessed data in an INDEX_ALLOCATION_BUFFER");
}

//Reads INDEX_ENTRY which is the same for $INDEX_ALLOCATION and $INDEX_ROOT.
size_t DirIndexProcessor::tryReadIndexEntry(byte* buf, size_t len)
{
	//Terminator entries are 16 bytes! Less than sizeof(INDEX_ENTRY).
	if (len < INDEX_ENTRY_MIN_SIZE) return 0;

	auto entry = (INDEX_ENTRY*)buf;
	if (len < entry->Length) return 0;

	if (entry->Flags & INDEX_ENTRY_END) {
		//Why do we have a separate INDEX_ENTRY_END? What about FirstFreeByte? Let's assert:
		assert(len == entry->Length, "INDEX_ENTRY_END not at the end of an index buffer.");
		return entry->Length;
	}

	//If this is an intermediate node we simply don't care
	if (entry->Flags & INDEX_ENTRY_NODE)
		return entry->Length;

	DirIndexEntry dirEntry;
	assert(entry->FileReference.mergedValue != 0, "Leaf index nodes must have valid references");
	dirEntry.segmentNo = entry->FileReference.segmentNumber();
	dirEntry.filename = AttrFilename(&entry->FileName).name();

	this->entries.push_back(dirEntry);
	return entry->Length;
}



DirEntryLoader::DirEntryLoader(Mft& mft)
	: MultiSegmentFileLoader(mft.vol), DirIndexProcessor(mft.vol)
{
}

void DirEntryLoader::loadSegment(FILE_RECORD_SEGMENT_HEADER* segment)
{
	if (segment->BaseFileRecordSegment.mergedValue == 0)
		this->isDir = (segment->Flags & FILE_FILE_NAME_INDEX_PRESENT);
	MultiSegmentFileLoader::loadSegment(segment);
	this->m_bitmapLoader.advance(); //In case it was non-resident
	this->advance();
}

void DirEntryLoader::processAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	MultiSegmentFileLoader::processAttr(attr);
	//Extract the file name too
	if (attr.TypeCode == $FILE_NAME)
		this->filename.update(attr);
	auto attrName = attrNameStr(&attr);
	if (attr.TypeCode == $BITMAP && attrName == "$I30") {
		this->processBitmapAttr(attr);
	}
	if (attr.TypeCode == $INDEX_ROOT && attrName == "$I30") {
		assert(attr.FormCode == RESIDENT_FORM);
		this->processIndexRoot(attr.ResidentValuePtr(), attr.Form.Resident.ValueLength);
	}
	if (attr.TypeCode == $INDEX_ALLOCATION && attrName == "$I30") {
 		this->addAttrChunk(&attr);
	}
	//Do not do advance() here as very often we'll lack $BITMAP or $INDEX_ROOT and simply read out all available data, blowing up the buffers.
	//Do it after the complete segment load.
	//This way if this is a single-segment entry, we simply process it after gathering all the info.
	//Multi-segment entries will still be parsed fine.
}


//It was a great idea to store everything in UTF-8 but now we need string comparison, damn
bool utf8_iequals(const std::string& str1, const std::string& str2) {
	auto to_utf16 = [](const std::string& utf8) {
		if (utf8.empty()) return std::wstring();
		int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8[0], (int)utf8.size(), NULL, 0);
		std::wstring wstrTo(size_needed, 0);
		MultiByteToWideChar(CP_UTF8, 0, &utf8[0], (int)utf8.size(), &wstrTo[0], size_needed);
		return wstrTo;
	};

	std::wstring wstr1 = to_utf16(str1);
	std::wstring wstr2 = to_utf16(str2);

	// Use LOCALE_NAME_INVARIANT for consistent cross-culture behavior
	// or a specific locale like L"en-US" for linguistic correctness.
	int result = CompareStringEx(
		LOCALE_NAME_INVARIANT,
		NORM_IGNORECASE,
		wstr1.c_str(), -1,
		wstr2.c_str(), -1,
		NULL, NULL, 0
	);

	return result == CSTR_EQUAL;
}


DirectoryTreeLoader::DirectoryTreeLoader(Mft& mft)
	: mft(mft)
{
}

MftDirEntry& DirectoryTreeLoader::get(SegmentNumber segmentNo)
{
	auto it = m_data.find(segmentNo);
	if (it == m_data.end()) {
		return m_data[segmentNo] = load(segmentNo);
	}
	return it->second;
}

MftDirEntry DirectoryTreeLoader::load(SegmentNumber segmentNo)
{
	MftDirEntry result;
	DirEntryLoader loader(mft);
	loader.load(mft, segmentNo);
	result.name = std::move(loader.filename.filename);
	for (auto& childEntry : loader.entries) {
		DirIndexEntry child;
		child.segmentNo = childEntry.segmentNo;
		child.filename = std::move(childEntry.filename);
		result.children.push_back(child);
	}
	loader.assert_all_processed();
	return result;
}

SegmentNumber DirectoryTreeLoader::traverse(const std::string& path)
{
	const std::string pathSep = "\\";
	std::string remainingPath = path;

	//Completely empty paths do NOT reference root, this requires at least one / or .
	if (remainingPath.empty()) {
		qWarning() << "traverse: Completely empty path cannot be resolved." << std::endl;
		return -1;
	}

	SegmentNumber currentSegment = 5;
	std::vector<SegmentNumber> segments {};

	size_t pos = 0;
	std::string token;
	while (!remainingPath.empty()) {
		auto pos = remainingPath.find(pathSep);
		if (pos != std::string::npos) {
			token = remainingPath.substr(0, pos);
			remainingPath.erase(0, pos + pathSep.length());
		}
		else {
			token = remainingPath;
			remainingPath.clear();
		}

		if (token == "") continue;
		if (token == ".") continue; //Simply "current dir", or root
		//Do minimal handling of these. Could have skipped going into dirs entirely!
		if (token == "..") {
			assert(segments.size() > 0, "Invalid path: .. above root");
			currentSegment = segments.back();
			segments.pop_back();
			continue;
		}

		//Query the current segment contents
		MftDirEntry& dirEntry = this->get(currentSegment);
		bool found = false;
		for (auto& childEntry : dirEntry.children) {
			if (utf8_iequals(childEntry.filename, token)) {
				segments.push_back(currentSegment);
				currentSegment = childEntry.segmentNo;
				found = true;
				break;
			}
		}

		if (!found) {
			qWarning() << "Cannot locate path component: " << token << std::endl;
			return -1;
		}
	}

	qDebug() << "Resolved " << path << " to " << currentSegment << "." << std::endl;
	return currentSegment;
}

std::unordered_set<SegmentNumber> DirectoryTreeLoader::traversePaths(const std::vector<std::string>& paths)
{
	std::unordered_set<SegmentNumber> results;
	for (auto& path : paths) {
		auto segmentNo = traverse(path);
		if (segmentNo == -1)
			qWarning() << "Path not found: " << path << std::endl;
		else
			results.insert(segmentNo);
	}
	return results;
}




/*
We should try to minimize direct segmentNos and prefer segmentRoots.
If we're adding a segment as a child of another segment, we should not add it to direct segments ever.
It's already excluded via its parent.
*/

void SegmentInclusionOptions::setTree(DirectoryTreeLoader* dirTree)
{
	this->dirTree = dirTree;
}

void SegmentInclusionOptions::resolvePaths()
{
	//All -files and -paths lists have to be traversed and added
	for (auto& segmentNo : dirTree->traversePaths(paths))
		segmentRoots.insert(segmentNo);
	for (auto& segmentNo : dirTree->traversePaths(files))
		segments.insert(segmentNo);
}

void SegmentInclusionOptions::resolve()
{
	this->resolvePaths();

	//Now that we have everything in two lists, we have to resolve subtrees

	//Add the root entries to the direct exclusion
	for (auto& segmentNo : segmentRoots)
		segments.insert(segmentNo);
	//From here on, loadSubtrees ONLY contains the children of exclusions or the roots already added directly.
	//So don't add anything to direct exclusions now.

	//Move segmentRoots to the queue and add them back as they are processed.
	std::unordered_set<SegmentNumber> loadSubtrees = std::move(segmentRoots);

	//Segments we've seen. Prevents duplicate processing and recursion. Can't just check segmentRoots as segments w/out children don't go there.
	std::unordered_set<SegmentNumber> visited {};

	//Process entries from loadSubtrees, adding new ones as we encounter them
	while (!loadSubtrees.empty()) {
		auto it = loadSubtrees.begin();
		auto segmentNo = *it;
		loadSubtrees.erase(it);

		auto pair = visited.insert(segmentNo);
		if (!pair.second)
			continue; //Already processed

		auto& entry = dirTree->get(segmentNo);
		for (auto& childEntry : entry.children) {
			if (segmentRoots.find(childEntry.segmentNo) == segmentRoots.end())
				loadSubtrees.insert(childEntry.segmentNo);
		}

		//If the entry has no children it's empty/not a dir. No point in checking against it.
		if (!entry.children.empty())
			segmentRoots.insert(segmentNo);
	}

	this->printDebugInfo();
}

void SegmentInclusionOptions::printDebugInfo()
{
	qDebug() << "Segment list:" << std::endl;
	for (auto& segmentNo : segments)
		qDebug() << " #" << segmentNo;
	qDebug() << std::endl;

	qDebug() << "SegmentRoot list:" << std::endl;
	for (auto& segmentNo : segmentRoots)
		qDebug() << " #" << segmentNo;
	qDebug() << std::endl;
}




