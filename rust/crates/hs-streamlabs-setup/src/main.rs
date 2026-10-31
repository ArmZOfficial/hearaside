use std::io::Write;

fn main() {
    // wide-character arguments, so a folder with non-ASCII letters (a Thai user name) works
    let args: Vec<String> = std::env::args_os()
        .skip(1)
        .map(|a| a.to_string_lossy().into_owned())
        .collect();
    let o = hs_streamlabs_setup::run(&args, &hs_streamlabs_setup::streamlabs_running);
    let _ = std::io::stdout().write_all(o.stdout.as_bytes());
    let _ = std::io::stderr().write_all(o.stderr.as_bytes());
    std::process::exit(o.code);
}
