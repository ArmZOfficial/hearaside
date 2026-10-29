fn main() {
    // The same page as the headless spike (rust/spikes/ui-slint), now inside a plug-in window.
    slint_build::compile("../../ui-slint/ui/track.slint").expect("compile track.slint");
}
