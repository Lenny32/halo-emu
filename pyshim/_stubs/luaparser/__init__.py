# Copyright (c) 2025 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0
"""Import-time stub for the `luaparser` PyPI package.

test_time.py declares luaparser in its PEP 723 header and imports it, but
never uses it. Under `uv run` the real package is installed per-script; the
emulator runner (`emulator/tools/run_emu_tests.py`) drives the tests with
plain python3 instead, and adds THIS directory to PYTHONPATH only when the
real luaparser is not importable — just enough to satisfy the import.

If a test ever actually uses luaparser, install the real package (or use uv);
this stub deliberately contains nothing.
"""
