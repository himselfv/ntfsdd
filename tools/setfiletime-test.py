import argparse
import os
import random
import string


"""
A tool to compare the effects of changing the file contents and then restoring its LastModificationTime with SetFileTime.
Usage:
1. Create the file:
  setfiletime-test.py test.txt

2. Find out its ID:
  fsutil file queryfileid test.txt
    MFT Record Index (Segment Number): The low 48 bits (the last 12 hex characters) represent the actual MFT segment number.
    Sequence Number: The high 16 bits (the first 4 hex characters) represent the sequence number, which NTFS increments every time an MFT record is reused.

3. Dump the segment:
  ntfs C: --shadow --dump-segment 15329 >>segment-15329-01.txt
  ntfs C: --shadow --print-segment 15329 >>segment-15329-01.txt

4. Run this script again to change the file and roll back FileTime:
  setfiletime-test.py test.txt

5. Dump the segment again:
  ntfs C: --shadow --dump-segment 15329 >>segment-15329-02.txt
  ntfs C: --shadow --print-segment 15329 >>segment-15329-02.txt

6. Compare with diff or WinMerge or something.

Similar script can be written for dirs but after you verify that LastChangeTime survives SetFileTime for files,
you can simply --dump-segment --print-segment the dir before and after your changes (create file, rename file, delete file),
and notice that LastChangeTime changes here too.
"""


parser = argparse.ArgumentParser(description='Changes the contents of the file and then tries to restore its filetimes so that its MFT looks identical.')
parser.add_argument('filename')
args = parser.parse_args()


def random_string(length):
    characters = string.ascii_letters + string.digits
    random_string = ''.join(random.choices(characters, k=length))
    return random_string

def get_times(filename):
	stats = os.stat(filename)
	print(f"Retrieved ftime: {stats.st_atime}, {stats.st_mtime}")
	return (stats.st_atime, stats.st_mtime)


# Has to be long enough that it doesn't fit in the MFT
# Better if it's random enough that we can meaningfully catch unrelated files by comparing their size.
FILE_DATA_SIZE=4321
new_data = random_string(FILE_DATA_SIZE)

# Safety so that we don't overwrite unrelated files
if not os.path.exists(args.filename):
	print("File does not exist, creating, re-run to actually test.")
	with open(args.filename, 'x') as fp:
		fp.write(new_data)
	exit(0)

if os.path.getsize(args.filename)!=FILE_DATA_SIZE:
	print("Error: file exists, different size, likely not our test file, will not overwrite, aborting")
	exit(-1)

# 1. Capture the original timestamps
# st_atime is 'Last Accessed', st_mtime is 'Last Modified'
original_times = get_times(args.filename)

# 2. Modify the file content
with open(args.filename, 'w', encoding='utf-8') as f:
    f.write(new_data)

# 3. Restore the original timestamps
os.utime(args.filename, original_times)
print("File modified and original timestamps restored successfully.")

get_times(args.filename)
