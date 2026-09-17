#!/usr/bin/env node

// Copyright 2026 moonbit-libyue contributors. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

// Fix typos in the vendored base headers, which come from an external
// submodule and cannot be fixed here.
//
// The no_destructor.h typo breaks the macOS build with newer clang (the
// member gets instantiated there), so this must run right after the
// submodule checkout in bootstrap.js, and again on the generated
// distribution in create_source_dist.js.
//
// Usage: node scripts/patch_vendored_headers.js [root]
//   root = .               patches base/... of the build tree (default)
//   root = out/Dist/source patches include/base/... of the distribution

const path = require('path')
const fs = require('fs')

const patches = [
  {
    file: path.join('allocator', 'partition_allocator', 'src',
                    'partition_alloc', 'partition_alloc_base',
                    'no_destructor.h'),
    from: 'return const_cast<PlacementStorage*>(this)->storage();',
    to: 'return reinterpret_cast<const T*>(storage_);',
  },
  {
    file: path.join('containers', 'id_map.h'),
    from: '      map_ = iter.map;\n      iter_ = iter.iter;',
    to: '      map_ = iter.map_;\n      iter_ = iter.iter_;',
  },
]

const root = process.argv[2] || '.'

for (const patch of patches) {
  for (const base of [path.join(root, 'base'), path.join(root, 'include', 'base')]) {
    const file = path.join(base, patch.file)
    if (!fs.existsSync(file))
      continue
    const text = fs.readFileSync(file, 'utf8')
    if (text.includes(patch.to))
      continue  // already fixed
    // The submodule may be checked out with CRLF line endings on Windows.
    const normalized = text.replace(/\r\n/g, '\n')
    if (!normalized.includes(patch.from)) {
      console.warn(`Warning: typo not found in ${file}, the submodule may have changed`)
      continue
    }
    fs.writeFileSync(file, normalized.replace(patch.from, patch.to))
    console.log(`Patched ${file}`)
  }
}
