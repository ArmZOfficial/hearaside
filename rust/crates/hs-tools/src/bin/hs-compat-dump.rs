use std::{env, fs, path::Path, process::ExitCode};

#[cfg(windows)]
#[path = "../vst3_windows.rs"]
mod vst3_windows;

#[cfg(windows)]
use vst3_windows::dump;

#[cfg(not(windows))]
fn dump(_plugin: &Path) -> Result<serde_json::Value, String> {
    Err("VST3 binary inspection currently requires Windows x64".into())
}

fn compare_fixture(actual: &serde_json::Value, fixture: &Path) -> Result<(), String> {
    let bytes = fs::read(fixture).map_err(|e| format!("{}: {e}", fixture.display()))?;
    let expected: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|e| format!("{}: {e}", fixture.display()))?;
    if actual != &expected {
        return Err(format!(
            "compatibility fixture differs: {}",
            fixture.display()
        ));
    }
    Ok(())
}

fn run() -> Result<(), String> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    let (plugin, fixture) = match args.as_slice() {
        [plugin] if plugin != "--help" => (plugin, None),
        [check, fixture, plugin] if check == "--check" => (plugin, Some(fixture)),
        _ => return Err("usage: hs-compat-dump [--check <fixture.json>] <plugin.vst3>".into()),
    };
    let result = dump(Path::new(plugin))?;
    if let Some(fixture) = fixture {
        compare_fixture(&result, Path::new(fixture))?;
        eprintln!("compatibility fixture matches");
    } else {
        let json = serde_json::to_string_pretty(&result).map_err(|e| e.to_string())?;
        println!("{json}");
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("hs-compat-dump: {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn compares_json_values_and_preserves_parameter_order() {
        let fixture = env::temp_dir().join(format!("hs-compat-dump-{}.json", std::process::id()));
        let expected = json!({"parameters": [{"id": 10}, {"id": 20}]});
        fs::write(&fixture, "{\"parameters\":[{\"id\":10},{\"id\":20}]}").unwrap();
        assert!(compare_fixture(&expected, &fixture).is_ok());
        assert!(
            compare_fixture(&json!({"parameters": [{"id": 20}, {"id": 10}]}), &fixture).is_err()
        );
        fs::write(&fixture, "not json").unwrap();
        assert!(compare_fixture(&expected, &fixture).is_err());
        fs::remove_file(&fixture).unwrap();
        assert!(compare_fixture(&expected, &fixture).is_err());
    }
}
