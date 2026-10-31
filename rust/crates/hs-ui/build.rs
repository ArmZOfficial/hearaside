//! Compiles the Slint sources. `ui/gallery.slint` imports every component, so one compile covers the library and the sample page.
fn main() {
    println!("cargo:rerun-if-changed=ui");
    println!("cargo:rerun-if-changed=../../../external/fonts");
    slint_build::compile("ui/gallery.slint").expect("compile ui/gallery.slint");
}
