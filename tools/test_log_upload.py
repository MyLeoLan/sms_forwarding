#!/usr/bin/env python3
"""在主机上编译 idf_log.cpp 中真实的时间前缀与增量收集函数并回归。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_logbuf/idf_log.cpp').read_text()


def function(name):
    start = source.rfind('\n', 0, source.index(name)) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]


code = '\n\n'.join([
    'static std::atomic<int> s_tz_offset_min{480};',
    function('void idf_log_set_tz_offset_min('),
    function('static void format_log_time('),
    function('uint32_t idf_log_current_seq('),
    function('uint32_t idf_log_collect_since('),
])
fixture = (ROOT / 'tests/log_upload_test.cpp').read_text().replace('// @真实函数@', code)
(ROOT / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='test-logup-', dir=ROOT / 'build') as temp:
    cpp = Path(temp) / 'test.cpp'
    binary = Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
