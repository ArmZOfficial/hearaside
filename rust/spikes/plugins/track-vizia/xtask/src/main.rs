//! `cargo xtask bundle <package> [cargo args]`. nice_plug_xtask::main() picks the outermost folder
//! with a Cargo.toml as the workspace (that would be rust/), so this calls its build and bundle steps
//! from this spike's own workspace instead.
use std::path::Path;

fn main() -> nice_plug_xtask::Result<()> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().expect("xtask lives inside the workspace");
    std::env::set_current_dir(root)?;
    let mut args = std::env::args().skip(1);
    if args.next().as_deref() != Some("bundle") {
        anyhow_free_exit("usage: cargo xtask bundle <package> [--release] [--target <triple>]");
    }
    let package = args.next().unwrap_or_else(|| anyhow_free_exit("missing package"));
    let rest: Vec<String> = args.collect();
    let universal = rest.iter().any(|a| a == "--universal");
    let cargo_args: Vec<String> = rest.into_iter().filter(|a| a != "--universal").collect();
    nice_plug_xtask::build(std::slice::from_ref(&package), &cargo_args)?;
    nice_plug_xtask::bundle(&root.join("target"), &package, &cargo_args, universal)
}

fn anyhow_free_exit(message: &str) -> ! {
    eprintln!("{message}");
    std::process::exit(2)
}
