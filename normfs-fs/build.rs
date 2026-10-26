use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let include_dir = manifest_dir.join("c/include");
    let sources = [
        manifest_dir.join("c/src/fs_plan.c"),
        manifest_dir.join("c/src/fs_dir.c"),
        manifest_dir.join("c/src/fs_sys.c"),
    ];

    println!("cargo:rerun-if-changed={}", include_dir.display());
    for source in &sources {
        println!("cargo:rerun-if-changed={}", source.display());
    }

    c_build()
        .files(&sources)
        .include(&include_dir)
        .opt_level(3)
        .warnings(false)
        .compile("normfs_fs_c");
}

/// cl has its own flags; /WX holds the C to the same no-warnings bar as -Werror.
fn c_build() -> cc::Build {
    let mut build = cc::Build::new();
    if build.get_compiler().is_like_msvc() {
        build.flag("/std:c11").flag("/W4").flag("/WX");
    } else {
        build
            .flag("-std=c99")
            .flag("-Wall")
            .flag("-Wextra")
            .flag("-Werror")
            .flag("-pedantic");
    }
    build
}
