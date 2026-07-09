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

fn linkPlatform(step: *std.Build.Step.Compile, target: std.Build.ResolvedTarget) void {
    switch (target.result.os.tag) {
        .windows => {
            step.linkSystemLibrary("ws2_32"); // Winsock (plaintext HTTP transport)
            step.linkSystemLibrary("winhttp"); // HTTPS transport (ph_tls.c)
        },
        // Linux: dl for dladdr (signal_crash module lookup); pthread for the
        // sender thread. glibc backtrace() lives in libc.
        .linux => {
            step.linkSystemLibrary("pthread");
            step.linkSystemLibrary("dl");
        },
        // macOS et al.: dladdr/backtrace are in libSystem; pthread is part of
        // libc, but requesting it explicitly is harmless and correct.
        else => step.linkSystemLibrary("pthread"),
    }
}

// Shared TUs compiled into the WASM module alongside the shim (ph_wasm.c).
// The native transport/queue/thread files are deliberately excluded; posthog-js
// owns delivery on the web.
const wasm_shared = [_][]const u8{
    "src/ph_wasm.c",
    "src/ph_props.c",
    "src/ph_json.c",
    "src/ph_str.c",
    "src/ph_util.c",
    "src/ph_serialize.c",
    "src/ph_time.c",
};

/// Locate emcc: tries $EMSDK/upstream/emscripten, ~/emsdk/upstream/emscripten,
/// then PATH. Returns null if not found (WASM steps then skip).
fn findEmcc(b: *std.Build) ?[]const u8 {
    const emcc_name: []const u8 = if (builtin.os.tag == .windows) "emcc.bat" else "emcc";
    const home_env: []const u8 = if (builtin.os.tag == .windows) "USERPROFILE" else "HOME";
    var extra: [2][]const u8 = undefined;
    var n: usize = 0;
    if (std.process.getEnvVarOwned(b.allocator, "EMSDK")) |emsdk| {
        extra[n] = std.fs.path.join(b.allocator, &.{ emsdk, "upstream", "emscripten" }) catch return null;
        n += 1;
    } else |_| {}
    if (std.process.getEnvVarOwned(b.allocator, home_env)) |home| {
        extra[n] = std.fs.path.join(b.allocator, &.{ home, "emsdk", "upstream", "emscripten" }) catch return null;
        n += 1;
    } else |_| {}
    return b.findProgram(&.{emcc_name}, extra[0..n]) catch null;
}

/// Locate node for the WASM behavioral harness: PATH first, then the version-
/// stamped directory emsdk bundles (<emsdk>/node/<ver>/bin/node).
fn findNode(b: *std.Build) ?[]const u8 {
    const node_name: []const u8 = if (builtin.os.tag == .windows) "node.exe" else "node";
    if (b.findProgram(&.{node_name}, &.{})) |p| {
        return p;
    } else |_| {}

    const home_env: []const u8 = if (builtin.os.tag == .windows) "USERPROFILE" else "HOME";
    var root: ?[]const u8 = null;
    if (std.process.getEnvVarOwned(b.allocator, "EMSDK")) |e| {
        root = e;
    } else |_| {}
    if (root == null) {
        if (std.process.getEnvVarOwned(b.allocator, home_env)) |home| {
            root = std.fs.path.join(b.allocator, &.{ home, "emsdk" }) catch null;
        } else |_| {}
    }
    if (root) |r| {
        const node_dir = std.fs.path.join(b.allocator, &.{ r, "node" }) catch return null;
        var dir = std.fs.cwd().openDir(node_dir, .{ .iterate = true }) catch return null;
        defer dir.close();
        var it = dir.iterate();
        while (it.next() catch null) |entry| {
            if (entry.kind != .directory) continue;
            const cand = std.fs.path.join(b.allocator, &.{ node_dir, entry.name, "bin", node_name }) catch continue;
            std.fs.cwd().access(cand, .{}) catch continue;
            return cand;
        }
    }
    return null;
}

/// Build the posthog-c static library. A consuming project that vendors this
/// repo as a submodule can `@import` this build.zig and call create() +
/// addIncludes() to link it - the usual way to vendor a C library into a Zig build.
pub fn create(
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
    lib.addIncludePath(b.path("include"));
    lib.addIncludePath(b.path("src"));
    lib.addIncludePath(b.path("third_party/sdefl"));
    lib.addCSourceFiles(.{ .files = &c_sources, .flags = &c_flags });
    lib.addCSourceFiles(.{ .files = &.{"src/ph_gzip.c"}, .flags = &gzip_flags });
    linkPlatform(lib, target);
    return lib;
}

/// Add the public include path so a consumer TU can `#include <posthog.h>`.
pub fn addIncludes(b: *std.Build, step: *std.Build.Step.Compile) void {
    step.addIncludePath(b.path("include"));
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
    tests.addIncludePath(b.path("include"));
    tests.addIncludePath(b.path("src"));
    tests.addCSourceFiles(.{ .files = &test_sources, .flags = &c_flags });
    tests.linkLibrary(lib);
    linkPlatform(tests, target);

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
            fz.addIncludePath(b.path("include"));
            fz.addIncludePath(b.path("src"));
            fz.addIncludePath(b.path("tests/fuzz"));
            fz.addCSourceFiles(.{ .files = &.{ "tests/fuzz/fuzz_run.c", src }, .flags = &c_flags });
            fz.linkLibrary(lib);
            linkPlatform(fz, target);
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
    example.addCSourceFiles(.{ .files = &.{"examples/quickstart.c"}, .flags = &c_flags });
    example.linkLibrary(lib);
    linkPlatform(example, target);
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
    example_cpp.addCSourceFiles(.{ .files = &.{"examples/quickstart.cpp"}, .flags = &.{ "-std=c++17", "-Wall", "-Wextra" } });
    example_cpp.linkLibrary(lib);
    linkPlatform(example_cpp, target);
    b.installArtifact(example_cpp);

    const run_example_cpp = b.addRunArtifact(example_cpp);
    b.step("run-example-cpp", "Run the C++ quickstart example").dependOn(&run_example_cpp.step);

    // -- WASM backend: `zig build test-wasm` -----------------------------
    // Compiles the shim + shared core with emcc, then runs the Node parity
    // harness (a mocked window.posthog). Skips with a warning if emcc/node are
    // absent, so a plain `zig build` on a machine without emsdk still works.
    const test_wasm_step = b.step("test-wasm", "Build the WASM backend with emcc and run the Node parity harness");
    if (findEmcc(b)) |emcc_path| {
        const compile = b.addSystemCommand(&.{emcc_path});
        compile.addArg(b.pathFromRoot("tests/wasm/test_wasm_main.c"));
        for (wasm_shared) |src| compile.addArg(b.pathFromRoot(src));
        compile.addArgs(&.{
            "-I",                    b.pathFromRoot("include"),
            "-I",                    b.pathFromRoot("src"),
            "-O1",                   "-sMODULARIZE=1",
            "-sEXPORT_NAME=createPH", "-sENVIRONMENT=node",
            "-sEXIT_RUNTIME=0",      "-sEXPORTED_FUNCTIONS=_main,_wasm_run_test",
            "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8",
            "-o",                    b.pathFromRoot("tests/wasm/test_wasm.mjs"),
        });
        if (findNode(b)) |node_path| {
            const run = b.addSystemCommand(&.{ node_path, b.pathFromRoot("tests/wasm/harness.mjs") });
            run.step.dependOn(&compile.step);
            test_wasm_step.dependOn(&run.step);
        } else {
            std.log.warn("node not found; WASM compiled but the behavioral harness was skipped", .{});
            test_wasm_step.dependOn(&compile.step);
        }
    } else {
        std.log.warn("emcc not found (install emsdk or set EMSDK); skipping WASM", .{});
    }
}
