#!/usr/bin/env python3
"""Exercise the actual file-hash functions on the host with ASan and UBSan.

Requires a C++ compiler and zlib development headers, but no display or SDL.
"""

import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <cstdio>
#include <string>
#include <sstream>
#include <iostream>
#include <zlib.h>
#include "md5.h"
namespace Utils {
namespace String {
std::string toHexString(unsigned int value) {
    std::ostringstream result;
    result << std::hex << value;
    return result.str();
}
}
namespace Zip {
struct ZipFile {
    static unsigned int computeCRC(unsigned int previous, const char* data, size_t size) {
        return crc32(previous, reinterpret_cast<const Bytef*>(data), size);
    }
};
}
namespace FileSystem {
'''

SUFFIX = r'''
}}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::cout << Utils::FileSystem::getFileCrc32(argv[1]) << "\n"
              << Utils::FileSystem::getFileMd5(argv[1]) << "\n";
}
'''


class FileHashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='turborama-hash-test-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'hashes'
        source = (ROOT / 'es-core/src/utils/FileSystemUtil.cpp').read_text()
        functions = []
        for name in ('getFileCrc32', 'getFileMd5'):
            match = re.search(r'\t\tstd::string ' + name + r'\(.*?\n\t\t\}', source, re.S)
            if not match:
                raise AssertionError('Cannot find production function: ' + name)
            functions.append(match[0])
        harness = Path(cls.tmp.name) / 'hashes.cpp'
        harness.write_text(PREFIX + '\n'.join(functions) + SUFFIX)
        subprocess.run(['c++', '-std=c++14', '-g', '-O1', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I', str(ROOT / 'es-core/src/utils'),
                        str(harness), str(ROOT / 'es-core/src/utils/md5.cpp'), '-lz',
                        '-o', str(cls.binary)], check=True)

    def run_hashes(self, path):
        result = subprocess.run([str(self.binary), str(path)], check=True, capture_output=True,
                                text=True, env={**os.environ, 'ASAN_OPTIONS': 'alloc_dealloc_mismatch=1'})
        return result.stdout.splitlines()

    def test_empty_and_multi_block_files(self):
        for content in (b'', b'123456789', bytes(range(256)) * 9000):
            with self.subTest(size=len(content)):
                path = Path(self.tmp.name) / 'input file.bin'
                path.write_bytes(content)
                crc, md5 = self.run_hashes(path)
                self.assertEqual(int(crc, 16), zlib.crc32(content))
                self.assertEqual(md5, hashlib.md5(content).hexdigest())

    def test_missing_file_returns_empty_hashes(self):
        self.assertEqual(self.run_hashes(Path(self.tmp.name) / 'missing'), ['', ''])


if __name__ == '__main__':
    unittest.main()
