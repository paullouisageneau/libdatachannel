#!/usr/bin/env python3

from kaitaistruct import KaitaiStruct, ValidationNotEqualError
import os
import getopt
import sys
import glob
from functools import reduce


class Ogg(KaitaiStruct):
    """Ogg is a popular media container format, which provides basic
    streaming / buffering mechanisms and is content-agnostic. Most
    popular codecs that are used within Ogg streams are Vorbis (thus
    making Ogg/Vorbis streams) and Theora (Ogg/Theora).

    Ogg stream is a sequence Ogg pages. They can be read sequentially,
    or one can jump into arbitrary stream location and scan for "OggS"
    sync code to find the beginning of a new Ogg page and continue
    decoding the stream contents from that one.
    """

    def __init__(self, _io, _parent=None, _root=None):
        KaitaiStruct.__init__(self, _io)
        self._parent = _parent
        self._root = _root if _root else self
        self._read()

    def _read(self):
        self.pages = []
        i = 0
        while not self._io.is_eof():
            self.pages.append(Ogg.Page(self._io, self, self._root))
            i += 1

    class Page(KaitaiStruct):
        """Ogg page is a basic unit of data in an Ogg bitstream, usually
        it's around 4-8 KB, with a maximum size of 65307 bytes.
        """

        def __init__(self, _io, _parent=None, _root=None):
            KaitaiStruct.__init__(self, _io)
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.sync_code = self._io.read_bytes(4)
            if not self.sync_code == b"\x4F\x67\x67\x53":
                raise ValidationNotEqualError(b"\x4F\x67\x67\x53", self.sync_code, self._io,
                                              u"/types/page/seq/0")
            self.version = self._io.read_bytes(1)
            if not self.version == b"\x00":
                raise ValidationNotEqualError(b"\x00", self.version, self._io, u"/types/page/seq/1")
            self.reserved1 = self._io.read_bits_int_be(5)
            self.is_end_of_stream = self._io.read_bits_int_be(1) != 0
            self.is_beginning_of_stream = self._io.read_bits_int_be(1) != 0
            self.is_continuation = self._io.read_bits_int_be(1) != 0
            self._io.align_to_byte()
            self.granule_pos = self._io.read_u8le()
            self.bitstream_serial = self._io.read_u4le()
            self.page_seq_num = self._io.read_u4le()
            self.crc32 = self._io.read_u4le()
            self.num_segments = self._io.read_u1()
            self.len_segments = [None] * self.num_segments
            for i in range(self.num_segments):
                self.len_segments[i] = self._io.read_u1()

            self.segments = [None] * self.num_segments
            for i in range(self.num_segments):
                self.segments[i] = self._io.read_bytes(self.len_segments[i])


def generate(input_file: str, output_dir: str, max_samples: int):
    if output_dir[-1] != "/":
        output_dir += "/"
    if os.path.isdir(output_dir):
        files_to_delete = glob.glob(output_dir + "*.opus")
        if len(files_to_delete) > 0:
            print("Remove following files?")
            for file in files_to_delete:
                print(file)
            response = input("Remove files? [y/n] ").lower()
            if response != "y" and response != "yes":
                print("Cancelling...")
                return
            print("Removing files")
            for file in files_to_delete:
                os.remove(file)
    else:
        os.makedirs(output_dir, exist_ok=True)
    audio_stream_file = "_audio_stream.ogg"
    if os.path.isfile(audio_stream_file):
        os.remove(audio_stream_file)
    os.system('ffmpeg -i {} -vn -ar 48000 -ac 2 -vbr off -acodec libopus -ab 64k {}'.format(input_file, audio_stream_file))

    data = Ogg.from_file(audio_stream_file)
    index = 0
    valid_pages = data.pages[2:]
    segments = list(reduce(lambda x, y: x + y.segments, valid_pages, []))[:max_samples]
    for segment in segments:
        name = "{}sample-{}.opus".format(output_dir, index)
        index += 1
        with open(name, 'wb') as file:
            assert len(list(segment)) == 160
            file.write(segment)
    os.remove(audio_stream_file)


def main(argv):
    input_file = None
    default_output_dir = "opus/"
    output_dir = default_output_dir
    max_samples = None
    try:
        opts, args = getopt.getopt(argv, "hi:o:m:", ["help", "ifile=", "odir=", "max="])
    except getopt.GetoptError:
        print('generate_opus.py -i <input_files> [-o <output_files>] [-m <max_samples>] [-h]')
        sys.exit(2)
    for opt, arg in opts:
        if opt in ("-h", "--help"):
            print("Usage: generate_opus.py -i <input_files> [-o <output_files>] [-m <max_samples>] [-h]")
            print("Arguments:")
            print("\t-i,--ifile: Input file")
            print("\t-o,--odir: Output directory (default: " + default_output_dir + ")")
            print("\t-m,--max: Maximum generated samples")
            print("\t-h,--help: Print this help and exit")
            sys.exit()
        elif opt in ("-i", "--ifile"):
            input_file = arg
        elif opt in ("-o", "--ofile"):
            output_dir = arg
        elif opt in ("-m", "--max"):
            max_samples = int(arg)
    if input_file is None:
        print("Missing argument -i")
        sys.exit(2)
    generate(input_file, output_dir, max_samples)


if __name__ == "__main__":
    main(sys.argv[1:])
