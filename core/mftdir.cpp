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
	memcpy((byte*)(&this->m_bitmap.data) + bytesProcessed, buf, len);
	bytesProcessed += len;
	return len;
}



DirIndexProcessor::DirIndexProcessor(Volume* vol)
	: AttributeCollectorProcessor(vol), m_bitmapLoader(vol, m_bitmap)
{
}

//Pass index root here. It's always resident, only one instance of it in the multi-segment file.
void DirIndexProcessor::processIndexRoot(void* data, size_t len)
{
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
	if (!m_haveRoot) return 0;

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
	assert(header->MultiSectorHeader.Signature == SIGNATURE_INDX);

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
	auto len = header->FirstFreeByte; //"Offset from FirstIndexEntry to the first free byte"
	
	//I know I just said we're not doing tryRead here, but...
	while (auto readSz = tryReadIndexEntry((byte*)data, len)) {
		data += readSz;
		len -= (ULONG)readSz;
	}
	assert_eq(len, 0, "Unprocessed data in an INDEX_ALLOCATION_BUFFER");
}

//Reads INDEX_ENTRY which is the same for $INDEX_ALLOCATION and $INDEX_ROOT.
size_t DirIndexProcessor::tryReadIndexEntry(byte* buf, size_t len)
{
	if (len < sizeof(INDEX_ENTRY)) return 0;
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
		this->advance();
	}
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
		result.children.push_back(childEntry.segmentNo);
	}
	return result;
}
