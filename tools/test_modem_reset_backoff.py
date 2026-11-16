#!/usr/bin/env python3
"""在主机上编译 idf_modem.cpp 中真实的自动硬重启退避函数并回归。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_modem/idf_modem.cpp').read_text()

begin = source.index('static constexpr uint32_t kAutoResetGapMinutes[]')
end = source.index('static bool handle_reset_request_if_any(void)', begin)
fixture = (ROOT / 'tests/modem_reset_backoff_test.cpp').read_text()
fixture = fixture.replace('// @真实函数@', source[begin:end])
(ROOT / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='test-backoff-', dir=ROOT / 'build') as temp:
    cpp = Path(temp) / 'test.cpp'
    binary = Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
