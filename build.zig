const std = @import("std");
const builtin = @import("builtin");

// The native backend. The wasm shim (src/ph_wasm.c) is compiled by an
// Emscripten build instead and is deliberately absent here.
const c_sources = [_][]const u8{
    "src/ph_str.c",
    "src/ph_json.c",
    "src/ph_util.c",
    "src/ph_thread.c",
    "src/ph_time.c",
    "src/ph_queue.c",
    "src/ph_props.c",
    "src/ph_serialize.c",
    "src/ph_jsonval.c",
    "src/ph_flags.c",
    "src/ph_ratelimit.c",
    "src/ph_http.c",
    "src/ph_tls.c",
    "src/ph_crash.c",
    "src/ph_core.c",
    "src/ph_exception.c",
    "src/ph_native.c",
};

const c_flags = [_][]const u8{ "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra" };

// third_party/sdefl is C90; compile ph_gzip.c (which pulls in its
// implementation) without -Wall/-Wextra so third-party warnings stay out.
const gzip_flags = [_][]const u8{ "-std=c11", "-D_GNU_SOURCE" };

const test_sources = [_][]const u8{
    "tests/test_main.c",
    "tests/mock_transport.c",
    "tests/test_json.c",
    "tests/test_props.c",
    "tests/test_queue.c",
    "tests/test_serialize.c",
    "tests/test_capture.c",
    "tests/test_scrub.c",
    "tests/test_exception.c",
    "tests/test_offline.c",
    "tests/test_jsonparse.c",
    "tests/test_flags.c",
    "tests/test_gzip.c",
    "tests/test_http.c",
    "tests/test_ratelimit.c",
    "tests/test_crash.c",
};

// `final_link` is true for executables (a real link step) and false for the
// static library. OpenSSL is linked only on a final binary: folding it into the
// static archive embeds libssl.so/libcrypto.so as archive members, which LLD
// warns about (harmless, but noisy). A downstream consumer of the static lib
// links -lssl/-lcrypto itself - the standard contract for a static library with
// a third-party shared dependency (see "Using it" in README.md).
fn linkPlatform(step: *std.Build.Step.Compile, target: std.Build.ResolvedTarget, final_link: bool) void {
    const m = step.root_module;
    switch (target.result.os.tag) {
        .windows => {
            m.linkSystemLibrary("ws2_32", .{}); // Winsock (plaintext HTTP transport)
            m.linkSystemLibrary("winhttp", .{}); // HTTPS transport (ph_tls.c)
            m.linkSystemLibrary("bcrypt", .{}); // OS entropy for UUID/reset-id salt
        },
        // Linux: dl for dladdr (signal_crash module lookup); pthread for the
        // sender thread. glibc backtrace() lives in libc. ssl/crypto are the
        // system OpenSSL (the HTTPS backend in ph_tls.c; needs libssl-dev),
        // linked only on the final binary - see the note above.
        .linux => {
            m.linkSystemLibrary("pthread", .{});
            m.linkSystemLibrary("dl", .{});
            if (final_link) {
                m.linkSystemLibrary("ssl", .{});
                m.linkSystemLibrary("crypto", .{});
            }
        },
        // macOS: Secure Transport (Security.framework) for TLS in ph_tls.c;
        // CoreFoundation for CFRelease of the SSL context. pthread is in libc.
        .macos => {
            m.linkSystemLibrary("pthread", .{});
            m.linkFramework("Security", .{});
            m.linkFramework("CoreFoundation", .{});
        },
        // Other Unix (Linux handled above): backtrace/dladdr live in libc.
        else => m.linkSystemLibrary("pthread", .{}),
    }
}

// Public Emscripten source/flag recipe. Build tests invoke emcc through this
// exact response file, so the downstream consumption surface cannot drift from
// the in-repo harness.
const wasm_recipe = "wasm/posthog-wasm.rsp";

fn addWasmCompile(b: *std.Build, emcc_path: []const u8) *std.Build.Step.Run {
    const compile = b.addSystemCommand(&.{emcc_path});
    // Paths inside the response file are relative to the package root.
    compile.setCwd(b.path("."));
    compile.addPrefixedFileArg("@", b.path(wasm_recipe));
    return compile;
}

/// Locate emcc: tries $EMSDK/upstream/emscripten, ~/emsdk/upstream/emscripten,
/// then PATH. Returns null if not found (WASM steps then skip).
fn findEmcc(b: *std.Build) ?[]const u8 {
    const emcc_name: []const u8 = if (builtin.os.tag == .windows) "emcc.bat" else "emcc";
    const home_env: []const u8 = if (builtin.os.tag == .windows) "USERPROFILE" else "HOME";
    var extra: [2][]const u8 = undefined;
    var n: usize = 0;
    if (b.graph.environ_map.get("EMSDK")) |emsdk| {
        extra[n] = std.fs.path.join(b.allocator, &.{ emsdk, "upstream", "emscripten" }) catch return null;
        n += 1;
    }
    if (b.graph.environ_map.get(home_env)) |home| {
        extra[n] = std.fs.path.join(b.allocator, &.{ home, "emsdk", "upstream", "emscripten" }) catch return null;
        n += 1;
    }
    return b.findProgram(&.{emcc_name}, extra[0..n]) catch null;
}

/// Locate node for the WASM behavioral harness: emsdk's EMSDK_NODE (the exact
/// bundled binary) first, then PATH.
fn findNode(b: *std.Build) ?[]const u8 {
    const node_name: []const u8 = if (builtin.os.tag == .windows) "node.exe" else "node";

    // emsdk exports EMSDK_NODE as the exact bundled node binary; prefer it. This
    // also sidesteps b.findProgram matching the un-spawnable `node` *directory*
    // that setup-emsdk leaves on $PATH. (A statFile guard here would be nicer,
    // but it drags __availability_version_check into the macOS build runner and
    // breaks its libSystem link.)
    if (b.graph.environ_map.get("EMSDK_NODE")) |n| {
        return n;
    }

    return b.findProgram(&.{node_name}, &.{}) catch null;
}

// Internal factory used by build() below. External projects should NOT call this
// (or addIncludes) via @import: b.path() resolves against the caller's build root,
// so it only works in-repo. To consume posthog-c, add it as a dependency and link
// `dep.artifact("posthog")` - see "Consuming it" in README.md.
fn create(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const lib = b.addLibrary(.{
        .name = "posthog",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    lib.root_module.addIncludePath(b.path("include"));
    lib.root_module.addIncludePath(b.path("src"));
    lib.root_module.addIncludePath(b.path("third_party/sdefl"));
    lib.root_module.addCSourceFiles(.{ .files = &c_sources, .flags = &c_flags });
    lib.root_module.addCSourceFiles(.{ .files = &.{"src/ph_gzip.c"}, .flags = &gzip_flags });
    linkPlatform(lib, target, false); // static archive: no third-party OpenSSL
    return lib;
}

// Add the public include path (in-repo helper; see the note on create()).
fn addIncludes(b: *std.Build, step: *std.Build.Step.Compile) void {
    step.root_module.addIncludePath(b.path("include"));
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const lib = create(b, target, optimize);
    lib.installHeader(b.path("include/posthog.h"), "posthog.h");
    lib.installHeader(b.path("include/posthog.hpp"), "posthog.hpp");
    b.installArtifact(lib);

    // -- Tests: `zig build test` -----------------------------------------
    const tests = b.addExecutable(.{
        .name = "posthog_tests",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    tests.root_module.addIncludePath(b.path("include"));
    tests.root_module.addIncludePath(b.path("src"));
    tests.root_module.addCSourceFiles(.{ .files = &test_sources, .flags = &c_flags });
    tests.root_module.linkLibrary(lib);
    linkPlatform(tests, target, true);

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Build and run the test suite");
    test_step.dependOn(&run_tests.step);

    // -- Fuzz targets: `zig build fuzz` ----------------------------------
    // Portable mutation harnesses over the two parsers that consume
    // network-controlled bytes: the /flags/ JSON parser and the HTTP response
    // parser. The targets use the libFuzzer entry point, so a coverage-guided
    // libFuzzer build (Linux) can drive them unchanged; here a built-in driver
    // (tests/fuzz/fuzz_run.c) mutates seed corpora. No sanitizer needed to catch
    // faults/hangs (that's how the JSON depth-cap bug was found).
    const fuzz_step = b.step("fuzz", "Build and run the parser fuzz targets");
    {
        const fuzz_srcs = [_][]const u8{ "tests/fuzz/fuzz_jsonval.c", "tests/fuzz/fuzz_http.c" };
        const fuzz_names = [_][]const u8{ "fuzz_jsonval", "fuzz_http" };
        for (fuzz_srcs, fuzz_names) |src, name| {
            const fz = b.addExecutable(.{
                .name = name,
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                }),
            });
            fz.root_module.addIncludePath(b.path("include"));
            fz.root_module.addIncludePath(b.path("src"));
            fz.root_module.addIncludePath(b.path("tests/fuzz"));
            fz.root_module.addCSourceFiles(.{ .files = &.{ "tests/fuzz/fuzz_run.c", src }, .flags = &c_flags });
            fz.root_module.linkLibrary(lib);
            linkPlatform(fz, target, true);
            const run_fz = b.addRunArtifact(fz);
            run_fz.addArg("50000"); // bounded iterations for a repeatable check
            fuzz_step.dependOn(&run_fz.step);
        }
    }

    // -- C example: `zig build run-example` ------------------------------
    const example = b.addExecutable(.{
        .name = "quickstart",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    addIncludes(b, example);
    example.root_module.addCSourceFiles(.{ .files = &.{"examples/quickstart.c"}, .flags = &c_flags });
    example.root_module.linkLibrary(lib);
    linkPlatform(example, target, true);
    b.installArtifact(example);

    const run_example = b.addRunArtifact(example);
    b.step("run-example", "Run the C quickstart example").dependOn(&run_example.step);

    // -- C++ example: `zig build run-example-cpp` ------------------------
    // Also serves as a compile check that the header-only C++ wrapper is valid.
    const example_cpp = b.addExecutable(.{
        .name = "quickstart_cpp",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .link_libcpp = true,
        }),
    });
    addIncludes(b, example_cpp);
    example_cpp.root_module.addCSourceFiles(.{ .files = &.{"examples/quickstart.cpp"}, .flags = &.{ "-std=c++17", "-Wall", "-Wextra" } });
    example_cpp.root_module.linkLibrary(lib);
    linkPlatform(example_cpp, target, true);
    b.installArtifact(example_cpp);

    const run_example_cpp = b.addRunArtifact(example_cpp);
    b.step("run-example-cpp", "Run the C++ quickstart example").dependOn(&run_example_cpp.step);

    // -- Opt-in live contract (credentials come only from the environment) --
    const live = b.addExecutable(.{
        .name = "posthog_live_contract",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    addIncludes(b, live);
    live.root_module.addCSourceFiles(.{ .files = &.{"tests/live_contract.c"}, .flags = &c_flags });
    live.root_module.linkLibrary(lib);
    linkPlatform(live, target, true);
    b.step("live-contract-compile", "Compile the opt-in live PostHog contract test")
        .dependOn(&live.step);
    const run_live = b.addRunArtifact(live);
    b.step("live-contract", "Run the opt-in live contract (requires POSTHOG_API_KEY)")
        .dependOn(&run_live.step);

    // -- WASM backend: `zig build test-wasm` -----------------------------
    // Compiles the public WASM recipe in strict C11, then runs the full Node
    // behavioral harness against both an ordinary and Closure-optimized build.
    // A separate public-header-only consumer fixture proves the recipe works
    // outside the internal harness. Ordinary `zig build` remains emsdk-free.
    const test_wasm_step = b.step("test-wasm", "Build and behavior-test normal/Closure WASM consumers");
    if (findEmcc(b)) |emcc_path| {
        const normal_compile = addWasmCompile(b, emcc_path);
        normal_compile.addFileArg(b.path("tests/wasm/test_wasm_main.c"));
        normal_compile.addArgs(&.{
            "-O1",
            "-Wall",
            "-Wextra",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=createPH",
            "-sENVIRONMENT=node",
            "-sEXIT_RUNTIME=0",
            "-sEXPORTED_FUNCTIONS=_main,_wasm_run_test",
            "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8",
            "-o",
            b.pathFromRoot("tests/wasm/test_wasm.mjs"),
        });

        const closure_compile = addWasmCompile(b, emcc_path);
        closure_compile.addFileArg(b.path("tests/wasm/test_wasm_main.c"));
        closure_compile.addArgs(&.{
            "-O3",
            "-Wall",
            "-Wextra",
            "--closure",
            "1",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=createPH",
            "-sENVIRONMENT=node",
            "-sEXIT_RUNTIME=0",
            "-sEXPORTED_FUNCTIONS=_main,_wasm_run_test",
            "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8",
            "-o",
            b.pathFromRoot("tests/wasm/test_wasm_closure.mjs"),
        });

        if (findNode(b)) |node_path| {
            const normal_run = b.addSystemCommand(&.{node_path});
            normal_run.addFileArg(b.path("tests/wasm/harness.mjs"));
            normal_run.addArg("./test_wasm.mjs");
            normal_run.step.dependOn(&normal_compile.step);
            test_wasm_step.dependOn(&normal_run.step);

            const closure_run = b.addSystemCommand(&.{node_path});
            closure_run.addFileArg(b.path("tests/wasm/harness.mjs"));
            closure_run.addArg("./test_wasm_closure.mjs");
            closure_run.step.dependOn(&closure_compile.step);
            test_wasm_step.dependOn(&closure_run.step);

            const consumer_run = b.addSystemCommand(&.{
                b.graph.zig_exe, "build", "run",
            });
            consumer_run.setCwd(b.path("tests/wasm/consumer"));
            consumer_run.addArg(b.fmt("-Demcc={s}", .{emcc_path}));
            consumer_run.addArg(b.fmt("-Dnode={s}", .{node_path}));
            test_wasm_step.dependOn(&consumer_run.step);
        } else {
            const fail = b.addFail("test-wasm requires Node (install Node or emsdk's bundled runtime)");
            test_wasm_step.dependOn(&fail.step);
        }
    } else {
        const fail = b.addFail("test-wasm requires emcc (install emsdk or set EMSDK)");
        test_wasm_step.dependOn(&fail.step);
    }
}
