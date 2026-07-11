const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const ph = b.dependency("posthog_c", .{ .target = target, .optimize = optimize });
    const exe = b.addExecutable(.{
        .name = "posthog_consumer_smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    exe.root_module.addCSourceFile(.{ .file = b.path("main.c"), .flags = &.{"-std=c11"} });
    exe.root_module.addIncludePath(ph.path("include"));
    exe.root_module.linkLibrary(ph.artifact("posthog"));
    // The static lib leaves its one third-party shared dependency to the final
    // binary: on Linux the TLS backend is the system OpenSSL, so link it here.
    if (target.result.os.tag == .linux) {
        exe.root_module.linkSystemLibrary("ssl", .{});
        exe.root_module.linkSystemLibrary("crypto", .{});
    }
    const run = b.addRunArtifact(exe);
    b.step("run", "Build, link, and run the downstream package smoke test")
        .dependOn(&run.step);
}
