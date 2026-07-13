const std = @import("std");

pub fn build(b: *std.Build) void {
    const emcc = b.option([]const u8, "emcc", "Absolute emcc executable path") orelse "emcc";
    const node = b.option([]const u8, "node", "Absolute Node executable path") orelse "node";
    const ph = b.dependency("posthog_c", .{});

    const compile = b.addSystemCommand(&.{emcc});
    // Source paths live inside the response file, so keep this smoke compile
    // side-effecting rather than let Zig cache it from the recipe token alone.
    compile.has_side_effects = true;
    // Recipe paths are package-root-relative, even when this fixture lives in
    // a separate package and consumes posthog-c through a dependency.
    compile.setCwd(ph.path("."));
    compile.addPrefixedFileArg("@", ph.path("wasm/posthog-wasm.rsp"));
    compile.addFileArg(b.path("main.c"));
    compile.addArgs(&.{
        "-Wall",            "-Wextra",         "-O1", "-sENVIRONMENT=node",
        "-sEXIT_RUNTIME=1", "-sSINGLE_FILE=1", "-o",
    });
    const output = compile.addOutputFileArg("posthog_wasm_consumer.js");

    const run = b.addSystemCommand(&.{node});
    run.addFileArg(output);
    b.step("run", "Compile and run the standalone WASM source consumer")
        .dependOn(&run.step);
}
