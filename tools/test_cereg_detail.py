#!/usr/bin/env python3
"""在主机上编译 idf_modem.cpp 中真实的 CEREG 拒绝原因解析函数并回归。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_modem/idf_modem.cpp').read_text()


def span(begin_marker, end_marker):
    begin = source.index(begin_marker)
    return source[begin:source.index(end_marker, begin)]


def function(name):
    start = source.rfind('\n', 0, source.index(name)) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]


code = '\n\n'.join([
    function('static bool parse_long_token('),
    function('static std::string line_containing('),
    span('struct CeregDetail {', '// 只接受独立的 COPS 查询行'),
])
fixture = (ROOT / 'tests/cereg_detail_test.cpp').read_text().replace('// @真实函数@', code)
(ROOT / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='test-cereg-', dir=ROOT / 'build') as temp:
    cpp = Path(temp) / 'test.cpp'
    binary = Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
