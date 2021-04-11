import struct
import sys
import os

def get_plain_filename(rawfile):
    (name, ext) = os.path.splitext(rawfile)
    return name + "_lat.txt"

def parse_raw(rawfile):
    outfile = open(get_plain_filename(rawfile), mode='w')
    infile = open(rawfile, mode="rb")
    byte = infile.read(24)
    while byte:
        (id, tx, rx) = struct.unpack("<3Q", byte)
        outfile.write("{0}\t{1}\n".format(id, int((rx - tx) / 2100)))
        byte = infile.read(24)
    infile.close()
    outfile.close()


if __name__ == "__main__":
    if (len(sys.argv) != 2):
        print("Usage: python ./parse_raw_record.py <raw record file>")
    else:
        rawfile = str(sys.argv[1])
        parse_raw(rawfile)
