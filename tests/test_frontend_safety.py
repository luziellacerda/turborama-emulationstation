#!/usr/bin/env python3
"""Host regression checks for production lookup/cleanup functions, without a UI."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

GENRE_PREFIX = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <map>
#include <string>
#include <vector>
namespace Utils { namespace String {
std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
    return value;
}
std::string trim(std::string value) {
    auto start = value.find_first_not_of(" ");
    return start == std::string::npos ? "" : value.substr(start, value.find_last_not_of(" ") - start + 1);
}
std::vector<std::string> splitAny(const std::string& value, const char* separators, bool) {
    std::vector<std::string> result;
    size_t start = 0, end;
    while ((end = value.find_first_of(separators, start)) != std::string::npos) {
        result.push_back(value.substr(start, end - start)); start = end + 1;
    }
    result.push_back(value.substr(start)); return result;
}
}}
struct GameGenre {};
struct Genres {
    static std::map<int, GameGenre*> mGenres;
    static std::map<std::string, int> mAllGenresNames;
    static GameGenre* fromGenreName(const std::string& name);
};
std::map<int, GameGenre*> Genres::mGenres;
std::map<std::string, int> Genres::mAllGenresNames;
'''

GENRE_SUFFIX = r'''
int main() {
    GameGenre action, platform;
    Genres::mGenres = {{1, &action}, {2, &platform}};
    Genres::mAllGenresNames = {{"ACTION", 1}, {"PLATFORM", 2}};
    assert(Genres::fromGenreName("action") == &action);
    assert(Genres::fromGenreName("Unknown / Action") == &action);
    assert(Genres::fromGenreName("Platform/Unknown") == &platform);
    assert(Genres::fromGenreName("Unknown") == nullptr);
    assert(Genres::fromGenreName("Unknown / Missing") == nullptr);
    assert(Genres::fromGenreName("") == nullptr);
    assert(Genres::mGenres.size() == 2);
}
'''

CHD_PREFIX = r'''
#include <cassert>
#include <cstddef>
#include <cstring>
#define CHD_EXPORT
#define EARLY_EXIT(x) do { (void)(x); goto cleanup; } while (0)
enum chd_error { CHDERR_NONE, CHDERR_INVALID_PARAMETER, CHDERR_FILE_NOT_FOUND, CHDERR_INVALID_DATA };
struct chd_file { void* file; };
struct chd_header {};
int opened = 0, closed = 0, token;
chd_error read_result = CHDERR_NONE, validate_result = CHDERR_NONE;
void* core_fopen(const char* name) {
    if (std::strcmp(name, "missing") == 0) return nullptr;
    ++opened; return &token;
}
void core_fclose(void* file) { assert(file == &token); ++closed; }
chd_error header_read(chd_file* file, chd_header*) { assert(file->file == &token); return read_result; }
chd_error header_validate(chd_header*) { return validate_result; }
'''

CHD_SUFFIX = r'''
int main() {
    chd_header header;
    assert(chd_read_header(nullptr, &header) == CHDERR_INVALID_PARAMETER);
    assert(chd_read_header("file", nullptr) == CHDERR_INVALID_PARAMETER);
    assert(opened == 0 && closed == 0);
    assert(chd_read_header("missing", &header) == CHDERR_FILE_NOT_FOUND);
    assert(opened == 0 && closed == 0);
    assert(chd_read_header("file", &header) == CHDERR_NONE);
    read_result = CHDERR_INVALID_DATA;
    assert(chd_read_header("file", &header) == CHDERR_INVALID_DATA);
    read_result = CHDERR_NONE;
    validate_result = CHDERR_INVALID_DATA;
    assert(chd_read_header("file", &header) == CHDERR_INVALID_DATA);
    assert(opened == 3 && closed == 3);
}
'''


class FrontendSafetyTests(unittest.TestCase):
    def compile_and_run(self, path, pattern, prefix, suffix):
        match = re.search(pattern, (ROOT / path).read_text(), re.S)
        self.assertIsNotNone(match, 'Production function was not found')
        with tempfile.TemporaryDirectory(prefix='turborama-safety-test-') as tmp:
            harness = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test'
            harness.write_text(prefix + match[0] + suffix)
            subprocess.run(['c++', '-std=c++14', '-g', '-O1', '-D_GLIBCXX_DEBUG',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-Werror=uninitialized', '-Werror=maybe-uninitialized',
                            str(harness), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_composite_genres_do_not_dereference_end_iterator(self):
        self.compile_and_run('es-app/src/Genres.cpp', r'GameGenre\* Genres::fromGenreName\(.*?\n\}',
                             GENRE_PREFIX, GENRE_SUFFIX)

    def test_invalid_chd_arguments_do_not_close_an_uninitialized_file(self):
        self.compile_and_run('external/libcheevos/libretro-common/src/formats/libchdr/libchdr_chd.c',
                             r'CHD_EXPORT chd_error chd_read_header\(.*?\n\}', CHD_PREFIX, CHD_SUFFIX)


if __name__ == '__main__':
    unittest.main()
