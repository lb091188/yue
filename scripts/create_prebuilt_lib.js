#!/usr/bin/env node

// Copyright 2026 moonbit-libyue contributors. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

// Compile the libyue static libraries from the source distribution and
// package them together with the headers, so consumers like the MoonBit
// bindings do not have to compile libyue locally.
//
// The compilation mirrors the configuration used by the consumers (C++20,
// static CRT on Windows, ARC split on macOS), see scripts/prebuilt.
//
// Must be run after scripts/create_source_dist.js.

const {version, targetOs, execSync, spawnSync} = require('./common')
const {createZip} = require('./zip_utils')

const path = require('path')
const fs = require('fs-extra')

main()

function main() {
  if (!fs.existsSync('out/Dist/source'))
    throw new Error('out/Dist/source does not exist, run create_source_dist.js first')
  // On macOS both architectures are built separately and merged with lipo,
  // which is more reliable than relying on universal flags reaching the
  // assembly units.
  const archs = targetOs == 'mac' ? ['arm64', 'x86_64'] : ['x64']
  for (const arch of archs)
    buildLibrary(arch)
  const archName = targetOs == 'mac' ? 'universal' : 'x64'
  packageLibrary(archs, archName)
}

function buildLibrary(arch) {
  const buildDir = path.join('out', 'Prebuilt', arch)
  fs.ensureDirSync(buildDir)
  const configure = ['cmake', '-S', 'scripts/prebuilt', '-B', buildDir,
                     '-DCMAKE_BUILD_TYPE=Release']
  if (targetOs == 'mac')
    configure.push(`-DCMAKE_OSX_ARCHITECTURES=${arch}`)
  if (targetOs == 'win')
    configure.push('-G', 'Visual Studio 17 2022', '-A', 'x64')
  if (spawnSync(configure[0], configure.slice(1), {stdio: 'inherit'}).status != 0)
    process.exit(1)
  const build = ['cmake', '--build', buildDir, '--parallel']
  if (targetOs == 'win')  // VS is a multi-config generator
    build.push('--config', 'Release')
  if (spawnSync(build[0], build.slice(1), {stdio: 'inherit'}).status != 0)
    process.exit(2)
}

function packageLibrary(archs, archName) {
  const pkg = path.join('out', 'Prebuilt', 'pkg')
  fs.emptyDirSync(pkg)
  fs.copySync('out/Dist/source/include', path.join(pkg, 'include'))
  fs.ensureDirSync(path.join(pkg, 'lib'))
  const libs = targetOs == 'win' ? ['yue.lib']
                                   : ['libyue.a', ...(targetOs == 'mac' ? ['libyue_noarc.a'] : [])]
  for (const lib of libs) {
    const inputs = archs.map((arch) => path.join('out', 'Prebuilt', arch, lib))
    const output = path.join(pkg, 'lib', lib)
    if (inputs.length > 1)
      execSync(`lipo -create ${inputs.join(' ')} -output ${output}`)
    else
      fs.copySync(inputs[0], output)
  }
  if (targetOs == 'win')
    fs.copySync(path.join('out', 'Dist', 'source', 'lib', 'WebView2Loader.dll'),
                path.join(pkg, 'lib', 'WebView2Loader.dll'))
  fs.writeFileSync(path.join(pkg, 'BUILD_INFO'),
                   `libyue ${version} prebuilt for ${targetOs} ${archName}\n`)
  createZip({withLicense: true})
    .addFile(pkg, pkg)
    .writeToFile(`libyue_prebuilt_${version}_${targetOs}_${archName}`)
  console.log(`Created out/Dist/libyue_prebuilt_${version}_${targetOs}_${archName}.zip`)
}
