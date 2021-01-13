/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*@flow*/
'use strict';
var Fs = require('fs');
var nThen = require('nthen');
var Codestyle = require('./Codestyle');
var Cp = require('./Cp');
var Spawn = require('child_process').spawn;
var FindPython = require('./FindPython');
var Builder = require('./builder');
const CjdnsTest = require('./CjdnsTest');
const GetVersion = require('./GetVersion');

var CFLAGS = process.env['CFLAGS'];
var LDFLAGS = process.env['LDFLAGS'];
var NO_MARCH_FLAG = ['arm', 'arm64', 'ppc', 'ppc64'];

Builder.configure({
    buildDir:       process.env['OUT_DIR'], // set by cargo
    systemName:     process.env['SYSTEM'] || process.platform,
    gcc:            process.env['CC'],
}, function (builder, waitFor) {

    builder.config.crossCompiling = process.env['CROSS'] !== undefined;
    let optimizeLevel = '-O3';

    builder.config.cflags.push(
        '-std=c99',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-Wno-pointer-sign',
        '-Wmissing-prototypes',
        '-pedantic',
        '-D', builder.config.systemName + '=1',
        '-Wno-unused-parameter',
        '-fomit-frame-pointer',
        '-ffunction-sections',
        '-fdata-sections',

        '-D', 'Log_' + (process.env['Log_LEVEL'] || 'DEBUG'),

        '-g',

        // f4 = 16 peers max, fixed width 4 bit
        // f8 = 241 peers max, fixed width 8 bit
        // v3x5x8 = 256 peers max, variable width, 3, 5 or 8 bits plus 1 or 2 bits of prefix
        // v4x8 = 256 peers max, variable width, 4, or 8 bits plus 1 bit prefix
        '-D', 'NumberCompress_TYPE=v3x5x8',

        // enable for safety (don't worry about speed, profiling shows they add ~nothing)
        '-D', 'Identity_CHECK=1',
        '-D', 'Allocator_USE_CANARIES=1',
        '-D', 'PARANOIA=1'
    );

    if (process.env["CJDNS_RELEASE_VERSION"]) {
        builder.config.version = '' + process.env["CJDNS_RELEASE_VERSION"];
    }

    if (process.env['SUBNODE']) { builder.config.cflags.push('-DSUBNODE=1'); }

    if (process.env['GCOV']) {
        builder.config.cflags.push('-fprofile-arcs', '-ftest-coverage');
        builder.config.ldflags.push('-fprofile-arcs', '-ftest-coverage');
    }

    var android = /android/i.test(builder.config.gcc);

    if (process.env['TESTING']) {
        builder.config.cflags.push('-D', 'TESTING=1');
    }

    if (process.env['ADDRESS_PREFIX']) {
        builder.config.cflags.push('-D', 'ADDRESS_PREFIX=' + process.env['ADDRESS_PREFIX']);
    }
    if (process.env['ADDRESS_PREFIX_BITS']) {
        builder.config.cflags.push('-D', 'ADDRESS_PREFIX_BITS=' + process.env['ADDRESS_PREFIX_BITS']);
    }

    if (!builder.config.crossCompiling) {
        if (NO_MARCH_FLAG.indexOf(process.arch) == -1) {
            builder.config.cflags.push('-march=native');
        }
    }

    if (builder.config.systemName === 'win32') {
        builder.config.cflags.push('-Wno-format');
    } else if (builder.config.systemName === 'linux') {
        builder.config.ldflags.push('-Wl,-z,relro,-z,now,-z,noexecstack');
        builder.config.cflags.push('-DHAS_ETH_INTERFACE=1');
    } else if (builder.config.systemName === 'darwin') {
        builder.config.cflags.push('-DHAS_ETH_INTERFACE=1');
    }

    if (process.env['NO_PIE'] === undefined && builder.config.systemName !== 'freebsd'
        && builder.config.systemName !== 'win32')
    {
        builder.config.cflags.push('-fPIE');

        // just using `-pie` on OS X >= 10.10 results in this warning:
        // clang: warning: argument unused during compilation: '-pie'
        if (builder.config.systemName !== "darwin")
        {
            builder.config.ldflags.push('-pie');
        } else {
            builder.config.ldflags.push('-Wl,-pie');
        }
    }

    if (builder.compilerType().isClang) {
        // blows up when preprocessing before js preprocessor
        builder.config.cflags.push(
            '-Wno-invalid-pp-token',
            '-Wno-dollar-in-identifier-extension',
            '-Wno-newline-eof',
            '-Wno-unused-value',

            // lots of places where depending on preprocessor conditions, a statement might be
            // a case of if (1 == 1)
            '-Wno-tautological-compare',

            '-Wno-error'
        );
        builder.config.cflags.slice(builder.config.cflags.indexOf('-Werror'), 1);
    } else {
        builder.config.cflags.push(
            '-fdiagnostics-color=always'
        );
    }

    // Install any user-defined CFLAGS. Necessary if you are messing about with building cnacl
    // with NEON on the BBB, or want to set -Os (OpenWrt)
    // Allow -O0 so while debugging all variables are present.
    if (CFLAGS) {
        var cflags = CFLAGS.split(' ');
        cflags.forEach(function(flag) {
             if (/^\-O[^02s]$/.test(flag)) {
                console.log("Skipping " + flag + ", assuming " + optimizeLevel + " instead.");
            } else if (/^\-O[02s]$/.test(flag)) {
                optimizeLevel = flag;
            } else {
                [].push.apply(builder.config.cflags, cflags);
            }
        });
    }

    builder.config.cflags.push(optimizeLevel);
    if (!/^\-O0$/.test(optimizeLevel)) {
        builder.config.cflags.push('-D_FORTIFY_SOURCE=2');
    }

    // We also need to pass various architecture/floating point flags to GCC when invoked as
    // a linker.
    if (LDFLAGS) {
        [].push.apply(builder.config.ldflags, LDFLAGS.split(' '));
    }

    if (android) {
        builder.config.cflags.push('-Dandroid=1');
    }

    var uclibc = process.env['UCLIBC'] == '1';
    var libssp;
    switch (process.env['SSP_SUPPORT']) {
        case 'y':
        case '1': libssp = true; break;
        case 'n':
        case '' :
        case '0': libssp = false; break;
        case undefined: break;
        default: throw new Error();
    }
    if (libssp === false) {
        console.log("Stack Smashing Protection (security feature) is disabled");
    } else if (builder.config.systemName == 'win32') {
        builder.config.libs.push('-lssp');
    } else if ((!uclibc && builder.config.systemName !== 'sunos') || libssp === true) {
        builder.config.cflags.push(
            // Broken GCC patch makes -fstack-protector-all not work
            // workaround is to give -fno-stack-protector first.
            // see: https://bugs.launchpad.net/ubuntu/+source/gcc-4.5/+bug/691722
            '-fno-stack-protector',
            '-fstack-protector-all',
            '-Wstack-protector'
        );

        // Static libssp provides __stack_chk_fail_local, which x86 needs in
        // order to avoid expensively looking up the location of __stack_chk_fail.
        var x86 = process.env['TARGET_ARCH'] == 'i386';
        if (uclibc) {
            if (x86) {
                builder.config.libs.push('-Wl,-Bstatic', '-lssp', '-Wl,-Bdynamic');
            } else {
                builder.config.libs.push('-lssp');
            }
        }
    } else {
        console.log("Stack Smashing Protection (security feature) is disabled");
    }

    if (process.env['Pipe_PREFIX']) {
        builder.config.cflags.push(
            '-D', 'Pipe_PREFIX="' + process.env['Pipe_PREFIX'] + '"'
        );
    }

    if (typeof(builder.config.cjdnsTest_files) === 'undefined') {
        CjdnsTest.generate(builder, process.env['SUBNODE'] !== '', waitFor());
    }

    nThen((w) => {
        if (builder.config.version) { return; }
        GetVersion(w(function(err, data) {
            if (!err) {
                builder.config.version = ('' + data).replace(/(\r\n|\n|\r)/gm, "");
            } else {
                builder.config.version = 'unknown';
            }
        }));
    }).nThen((w) => {
        builder.config.cflags.push('-D', 'CJD_PACKAGE_VERSION="' + builder.config.version + '"');
    }).nThen(waitFor());

    var dependencyDir = builder.config.buildDir + '/dependencies';
    var libuvLib = dependencyDir + '/libuv/out/Release/libuv.a';
    if (['win32', 'netbsd'].indexOf(builder.config.systemName) >= 0) {//this might be needed for other BSDs
        libuvLib = dependencyDir + '/libuv/out/Release/obj.target/libuv.a';
    }

    // Build dependencies
    let foundSodium = false;
    nThen(function (waitFor) {

        Fs.exists(dependencyDir, waitFor(function (exists) {
            if (exists) { return; }

            console.log("Copy dependencies");
            Cp('./node_build/dependencies', dependencyDir, waitFor());
        }));

    }).nThen(function (waitFor) {

        const dir = `${builder.config.buildDir}/../..`;
        Fs.readdir(dir, waitFor((err, ret) => {
            if (err) { throw err; }
            ret.forEach((f) => {
                if (!/^libsodium-sys-/.test(f)) { return; }
                const inclPath = `${dir}/${f}/out/source/libsodium/src/libsodium/include`;
                Fs.readdir(inclPath, waitFor((err, ret) => {
                    if (foundSodium) { return; }
                    if (err && err.code === 'ENOENT') { return; }
                    if (err) { throw err; }
                    builder.config.includeDirs.push(inclPath);
                    foundSodium = true;
                }));
            });
        }));

    }).nThen(function (waitFor) {

        if (!foundSodium) {
            throw new Error("Unable to find a path to libsodium headers");
        }

        builder.config.libs.push(libuvLib);
        if (!android) {
            builder.config.libs.push('-lpthread');
        }

        if (builder.config.systemName === 'win32') {
            builder.config.libs.push(
                '-lws2_32',
                '-lpsapi',   // GetProcessMemoryInfo()
                '-liphlpapi' // GetAdapterAddresses()
            );
        } else if (builder.config.systemName === 'linux' && !android) {
            builder.config.libs.push('-lrt'); // clock_gettime()
        } else if (builder.config.systemName === 'darwin') {
            builder.config.libs.push('-framework', 'CoreServices');
        } else if (['freebsd', 'openbsd', 'netbsd'].indexOf(builder.config.systemName) >= 0) {
            builder.config.cflags.push('-Wno-overlength-strings');
            builder.config.libs.push('-lkvm');
        } else if (builder.config.systemName === 'sunos') {
            builder.config.libs.push(
                '-lsocket',
                '-lsendfile',
                '-lkstat',
                '-lnsl'
            );
        }

        builder.config.includeDirs.push(dependencyDir + '/libuv/include/');

        var libuvBuilt;
        var python;
        nThen(function (waitFor) {

            Fs.exists(libuvLib, waitFor(function (exists) {
                if (exists) { libuvBuilt = true; }
            }));

        }).nThen(function (waitFor) {

            if (libuvBuilt) { return; }
            FindPython.find(builder.tmpFile(), waitFor(function (err, pythonExec) {
                if (err) { throw err; }
                python = pythonExec;
            }));

        }).nThen(function (waitFor) {

            if (libuvBuilt) { return; }
            console.log("Build Libuv");
            var cwd = process.cwd();
            process.chdir(dependencyDir + '/libuv/');

            var args = ['./gyp_uv.py'];
            var env = process.env;
            env.CC = builder.config.gcc;

            if (env.TARGET_ARCH) {
                args.push('-Dtarget_arch=' + env.TARGET_ARCH);
            }

            //args.push('--root-target=libuv');
            if (android) {
                args.push('-DOS=android');
                args.push('-f', 'make-linux');
            }

            if (builder.config.systemName === 'win32') {
                args.push('-DOS=win');
                args.push('-f', 'make-linux');
            }

            if (env.GYP_ADDITIONAL_ARGS) {
                args.push.apply(args, env.GYP_ADDITIONAL_ARGS.split(' '));
            }

            if (['freebsd', 'openbsd', 'netbsd'].indexOf(builder.config.systemName) !== -1) {
                // This platform lacks a functioning sem_open implementation, therefore...
                args.push('--no-parallel');
                args.push('-DOS=' + builder.config.systemName);
            }

            var gyp = Spawn(python, args, {env:env, stdio:'inherit'});
            gyp.on('error', function () {
                console.error("couldn't launch gyp [" + python + "]");
            });
            gyp.on('close', waitFor(function () {
                var args = [
                    '-j', ''+builder.config.jobs,
                    '-C', 'out',
                    'BUILDTYPE=Release',
                    'CC=' + builder.config.gcc,
                    'CXX=' + builder.config.gcc,
                    'V=1'
                ];
                var cflags = [optimizeLevel, '-DNO_EMFILE_TRICK=1'];

                if (!/^\-O0$/.test(optimizeLevel)) {
                    cflags.push('-D_FORTIFY_SOURCE=2');
                }

                if (!(/darwin|win32/i.test(builder.config.systemName))) {
                    cflags.push('-fPIC');
                }
                args.push('CFLAGS=' + cflags.join(' '));

                var makeCommand = ['freebsd', 'openbsd', 'netbsd'].indexOf(builder.config.systemName) >= 0 ? 'gmake' : 'make';
                var make = Spawn(makeCommand, args, {stdio: 'inherit'});

                make.on('error', function (err) {
                    if (err.code === 'ENOENT') {
                        console.error('\x1b[1;31mError: ' + makeCommand + ' is required!\x1b[0m');
                    } else {
                        console.error(
                            '\x1b[1;31mFail run ' + process.cwd() + ': ' + makeCommand + ' '
                            + args.join(' ') + '\x1b[0m'
                        );
                        console.error('Message:', err);
                    }
                    waitFor.abort();
                });

                make.on('close', waitFor(function () {
                    process.chdir(cwd);
                }));
            }));

        }).nThen((w) => {

            Fs.exists(libuvLib, waitFor((exists) => {
                if (!exists) {
                    throw new Error("Libuv build failed");
                }
            }));

        }).nThen(waitFor());

    }).nThen(waitFor());

}).build(function (builder, waitFor) {

    builder.buildLibrary('client/cjdroute2.c');

    builder.buildLibrary('contrib/c/publictoip6.c');
    builder.buildLibrary('contrib/c/privatetopublic.c');
    builder.buildLibrary('contrib/c/sybilsim.c');
    builder.buildLibrary('contrib/c/makekeys.c');
    builder.buildLibrary('contrib/c/mkpasswd.c');

    builder.buildLibrary('crypto/random/randombytes.c');

    builder.buildLibrary('rust/cjdns_sys/cffi.h');

    builder.lintFiles(function (fileName, file, callback) {
        if (/dependencies/.test(fileName) ||
            /crypto\/sign/.test(fileName) ||
            /.ffi\.h/.test(fileName)
        ) {
            callback('', false);
            return;
        }

        Codestyle.lint(fileName, file, callback);
    });

    builder.buildLibrary('test/testcjdroute.c');

}).failure(function (builder, waitFor) {

    console.log('\x1b[1;31mFailed to build cjdns.\x1b[0m');
    process.exit(1);

});