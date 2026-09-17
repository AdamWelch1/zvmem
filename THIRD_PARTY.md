# Third-Party Components

zvmem itself is licensed under the MIT License — see `LICENSE`.

The binary packages (`zvmem_static.tar.gz`, `zvmem_dynamic.tar.gz`) bundle code from
**[zvec](https://github.com/alibaba/zvec) v0.7.0**, licensed under the Apache License,
Version 2.0 (verbatim zvec NOTICE reproduced below). zvec incorporates the following
third-party components, each distributed under its own license:

| Component | Version | License |
|---|---|---|
| ANTLR4 | ~4.8 (pinned commit) | BSD-3-Clause |
| Apache Arrow (+ Parquet) | 21.0.0 | Apache-2.0 (+ LLVM exception) |
| cppjieba | 5.6.7 | MIT |
| CRoaring | 2.0.4 | Apache-2.0 / MIT (dual) |
| FastPFOR | 0.4.0 | Apache-2.0 |
| gflags | 2.2.2 | BSD (Google) |
| glog | 0.5.0 | BSD-3-Clause (Google) |
| googletest | 1.10.0 | BSD-3-Clause (test-only; not expected in shipped binaries) |
| limonp | v1.0.2 | MIT |
| lz4 (lib only) | 1.9.4 | BSD-2-Clause |
| magic_enum | 0.9.7 | MIT |
| RaBitQ-Library | 0.1 | Apache-2.0 |
| RocksDB | 8.1.1 | Apache-2.0 (LevelDB portion: new-BSD) |
| Snowball stemmers | 3.1.1 | free-use notice (M. Porter; see upstream COPYING) |
| sparsehash | 2.0.4 | BSD (Google) |
| utf8proc | 2.11.3 | MIT |
| yaml-cpp | 0.6.3 | MIT |

Also compiled into the binary via Arrow's dependency chain: Apache Thrift
(Apache-2.0), RE2 (BSD-3-Clause), zlib (zlib license), Boost headers only (BSL 1.0).
`nlohmann/json` v3.11.3 (MIT) is used directly by zvmem itself.

System libraries `libcurl`, `libstdc++` and `libc` are runtime prerequisites, not bundled.

Full license texts: see each upstream project's repository; the two components whose
license notices must accompany redistribution (Unicode data, pyglass) are reproduced
verbatim in the zvec NOTICE section below.

---

## zvec NOTICE — verbatim (alibaba/zvec v0.7.0)

zvec
Copyright 2025-present the zvec project

This product is licensed under the Apache License, Version 2.0 (see the LICENSE
file). It includes third-party software components that are distributed under
their own licenses, as listed below.

================================================================================
Third-Party Components
================================================================================

--------------------------------------------------------------------------------
Unicode Character Database
--------------------------------------------------------------------------------
Project: Unicode Character Database
Homepage: https://www.unicode.org/
License:  Unicode License V3
Used in:  src/db/index/column/fts_column/tokenizer/standard_tokenizer_unicode.inc

The generated standard tokenizer lookup tables are derived from Unicode 17.0.0
data files: auxiliary/WordBreakProperty.txt, emoji/emoji-data.txt,
LineBreak.txt, and Scripts.txt.

Unicode License V3 copyright and permission notice:

    UNICODE LICENSE V3 COPYRIGHT AND PERMISSION NOTICE
    Copyright © 1991-2026 Unicode, Inc.
    NOTICE TO USER: Carefully read the following legal agreement. BY DOWNLOADING,
    INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR SOFTWARE, YOU
    UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE TERMS AND
    CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT DOWNLOAD, INSTALL,
    COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of data files and any associated documentation (the "Data Files") or software
    and any associated documentation (the "Software") to deal in the Data Files
    or Software without restriction, including without limitation the rights to
    use, copy, modify, merge, publish, distribute, and/or sell copies of the Data
    Files or Software, and to permit persons to whom the Data Files or Software
    are furnished to do so, provided that either (a) this copyright and
    permission notice appear with all copies of the Data Files or Software, or
    (b) this copyright and permission notice appear in associated Documentation.

    THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
    KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
    THIRD PARTY RIGHTS.

    IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE BE
    LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR
    ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
    IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA FILES OR SOFTWARE.

    Except as contained in this notice, the name of a copyright holder shall not
    be used in advertising or otherwise to promote the sale, use or other
    dealings in these Data Files or Software without prior written authorization
    of the copyright holder.

--------------------------------------------------------------------------------
pyglass
--------------------------------------------------------------------------------
Project: pyglass — Graph Library for Approximate Similarity Search
Homepage: https://github.com/zilliztech/pyglass
License:  MIT License
Used in:  src/core/utility/linear_pool.h

The LinearPool implementation (and the accompanying Neighbor / Bitset helpers)
in src/core/utility/linear_pool.h is adapted from pyglass, with modifications
(a BlockHeap-compatible reset()/push_block() interface and the use of
MemoryHelper for huge-page-backed allocation). The related BlockHeap design in
src/core/utility/block_heap.{h,cc} is also derived from pyglass.

Original license text:

    MIT License

    Copyright (c) 2023 zh Wang

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
