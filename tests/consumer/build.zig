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
    exe.addCSourceFile(.{ .file = b.path("main.c"), .flags = &.{"-std=c11"} });
    exe.addIncludePath(ph.path("include"));
    exe.linkLibrary(ph.artifact("posthog"));
    const run = b.addRunArtifact(exe);
    b.step("run", "Build, link, and run the downstream package smoke test")
        .dependOn(&run.step);
}
