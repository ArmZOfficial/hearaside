use serde_json::{Value, json};
use std::{
    ffi::{CStr, c_char, c_void},
    mem,
    os::windows::ffi::OsStrExt,
    path::{Path, PathBuf},
    ptr,
};
use vst3::{
    Class, ComPtr, ComWrapper, Interface,
    Steinberg::{Vst::*, *},
};

#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryExW(name: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
}

#[link(name = "ole32")]
unsafe extern "system" {
    fn CoInitializeEx(reserved: *mut c_void, mode: u32) -> i32;
    fn CoUninitialize();
}

struct Apartment;
impl Apartment {
    fn new() -> Result<Self, String> {
        // SAFETY: Called once on the main thread, with null reserved and COINIT_APARTMENTTHREADED.
        let status = unsafe { CoInitializeEx(ptr::null_mut(), 2) };
        if status < 0 {
            return Err(format!("CoInitializeEx failed: {status:#x}"));
        }
        Ok(Self)
    }
}
impl Drop for Apartment {
    fn drop(&mut self) {
        // SAFETY: Balances this thread's successful CoInitializeEx after all plug-in objects drop.
        unsafe { CoUninitialize() };
    }
}

struct Module {
    handle: *mut c_void,
    exit: Option<unsafe extern "system" fn() -> bool>,
}
impl Module {
    fn load(path: &Path) -> Result<Self, String> {
        let path = binary_path(path)?;
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        // SAFETY: Absolute, NUL-terminated path; dependency search is limited to the DLL and default directories.
        let handle = unsafe { LoadLibraryExW(wide.as_ptr(), ptr::null_mut(), 0x100 | 0x1000) };
        if handle.is_null() {
            return Err(format!(
                "{}: {}",
                path.display(),
                std::io::Error::last_os_error()
            ));
        }
        let mut module = Self { handle, exit: None };
        let init = module.symbol(c"InitDll")?;
        let exit = module.symbol(c"ExitDll")?;
        // SAFETY: VST3 Windows ABI defines these exports as no-argument bool functions with system calling convention.
        let init: unsafe extern "system" fn() -> bool = unsafe { mem::transmute(init) };
        // SAFETY: ExitDll has the matching VST3 Windows ABI signature.
        let exit: unsafe extern "system" fn() -> bool = unsafe { mem::transmute(exit) };
        // SAFETY: Valid export from the live module, called once before accessing its factory.
        if !unsafe { init() } {
            return Err("InitDll failed".into());
        }
        module.exit = Some(exit);
        Ok(module)
    }
    fn symbol(&self, name: &CStr) -> Result<*mut c_void, String> {
        // SAFETY: The module remains loaded and name is NUL-terminated.
        let pointer = unsafe { GetProcAddress(self.handle, name.as_ptr()) };
        if pointer.is_null() {
            return Err(format!("missing export: {}", name.to_string_lossy()));
        }
        Ok(pointer)
    }
    fn factory(&self) -> Result<ComPtr<IPluginFactory>, String> {
        let address = self.symbol(c"GetPluginFactory")?;
        // SAFETY: VST3 specifies the factory export's exact signature; module stays loaded through factory release.
        let get: unsafe extern "system" fn() -> *mut IPluginFactory =
            unsafe { mem::transmute(address) };
        // SAFETY: GetPluginFactory returns one owned COM reference, adopted without addRef.
        unsafe { ComPtr::from_raw(get()) }.ok_or_else(|| "GetPluginFactory returned null".into())
    }
}
impl Drop for Module {
    fn drop(&mut self) {
        if let Some(exit) = self.exit {
            // SAFETY: All factory and object references have already dropped before the module guard.
            unsafe { exit() };
        }
        // SAFETY: Releases exactly the handle acquired by LoadLibraryExW.
        unsafe { FreeLibrary(self.handle) };
    }
}

fn binary_path(path: &Path) -> Result<PathBuf, String> {
    let path = path
        .canonicalize()
        .map_err(|e| format!("{}: {e}", path.display()))?;
    if path.is_file() {
        return Ok(path);
    }
    let stem = path.file_stem().ok_or("bundle has no name")?;
    let binary = path
        .join("Contents/x86_64-win")
        .join(stem)
        .with_extension("vst3");
    if !binary.is_file() {
        return Err(format!("VST3 x64 binary not found: {}", binary.display()));
    }
    Ok(binary)
}

struct Host;
impl Class for Host {
    type Interfaces = (IHostApplication,);
}
impl IHostApplicationTrait for Host {
    unsafe fn getName(&self, name: *mut String128) -> tresult {
        if name.is_null() {
            return kInvalidArgument;
        }
        let mut text = [0; 128];
        for (out, value) in text.iter_mut().zip("hs-compat-dump".encode_utf16()) {
            *out = value as TChar;
        }
        // SAFETY: VST3 requires the caller to supply a writable String128; null was rejected above.
        unsafe { name.write(text) };
        kResultOk
    }
    unsafe fn createInstance(
        &self,
        _cid: *mut TUID,
        _iid: *mut TUID,
        object: *mut *mut c_void,
    ) -> tresult {
        if object.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: Valid output pointer from the VST3 host call; unsupported objects return null.
        unsafe { object.write(ptr::null_mut()) };
        kNoInterface
    }
}

struct Initialized(ComPtr<IPluginBase>);
impl Initialized {
    fn new(base: ComPtr<IPluginBase>, host: *mut FUnknown) -> Result<Self, String> {
        // SAFETY: Both COM objects remain alive for the entire initialized instance lifetime.
        status(unsafe { base.initialize(host) }, "initialize")?;
        Ok(Self(base))
    }
}
impl Drop for Initialized {
    fn drop(&mut self) {
        // SAFETY: Balances exactly one successful initialize before releasing the object's COM reference.
        unsafe { self.0.terminate() };
    }
}

struct Connection {
    component: ComPtr<IConnectionPoint>,
    controller: ComPtr<IConnectionPoint>,
}

impl Connection {
    fn new(
        component: &ComPtr<IComponent>,
        controller: &ComPtr<IEditController>,
    ) -> Result<Self, String> {
        let component = component
            .cast::<IConnectionPoint>()
            .ok_or("component lacks IConnectionPoint")?;
        let controller = controller
            .cast::<IConnectionPoint>()
            .ok_or("controller lacks IConnectionPoint")?;
        status(
            // SAFETY: Both initialized objects remain referenced until this connection is disconnected.
            unsafe { component.connect(controller.as_ptr()) },
            "component connect",
        )?;
        if let Err(error) = status(
            // SAFETY: Live initialized COM objects, called on the main thread after the first connection.
            unsafe { controller.connect(component.as_ptr()) },
            "controller connect",
        ) {
            // SAFETY: Balances the successful first connection when the second call fails.
            unsafe { component.disconnect(controller.as_ptr()) };
            return Err(error);
        }
        Ok(Self {
            component,
            controller,
        })
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        // SAFETY: Both objects are live and initialized; disconnect before either terminate call.
        unsafe {
            self.controller.disconnect(self.component.as_ptr());
            self.component.disconnect(self.controller.as_ptr());
        }
    }
}

fn status(result: tresult, operation: &str) -> Result<(), String> {
    if result == kResultOk {
        Ok(())
    } else {
        Err(format!("{operation} failed: {result:#x}"))
    }
}

fn create<I: Interface>(factory: &ComPtr<IPluginFactory>, cid: &TUID) -> Result<ComPtr<I>, String> {
    let mut pointer = ptr::null_mut();
    status(
        // SAFETY: The factory is live; class/interface IDs each contain 16 bytes; output is writable.
        unsafe { factory.createInstance(cid.as_ptr(), I::IID.as_ptr().cast(), &mut pointer) },
        "createInstance",
    )?;
    // SAFETY: Successful createInstance transfers one reference for precisely the requested interface.
    unsafe { ComPtr::from_raw(pointer.cast()) }.ok_or_else(|| "createInstance returned null".into())
}

fn narrow(chars: &[c_char]) -> Result<String, String> {
    let bytes: Vec<u8> = chars
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8(bytes).map_err(|e| format!("invalid UTF-8 in VST3 metadata: {e}"))
}
fn wide(chars: &[TChar]) -> Result<String, String> {
    let chars: Vec<u16> = chars.iter().take_while(|&&c| c != 0).copied().collect();
    String::from_utf16(&chars).map_err(|e| format!("invalid UTF-16 in VST3 metadata: {e}"))
}
fn id_bytes(cid: &TUID) -> Vec<u8> {
    cid.iter().map(|&v| v as u8).collect()
}

fn parameters(
    factory: &ComPtr<IPluginFactory>,
    cid: &TUID,
    host: *mut FUnknown,
) -> Result<Value, String> {
    let component: ComPtr<IComponent> = create(factory, cid)?;
    let _component_init = Initialized::new(
        component.cast().ok_or("IComponent lacks IPluginBase")?,
        host,
    )?;
    let controller;
    let _controller_init;
    let mut controller_cid = [0; 16];
    if let Some(combined) = component.cast::<IEditController>() {
        controller = combined;
        _controller_init = None;
    } else {
        status(
            // SAFETY: Component is initialized and controller_cid is a writable 16-byte TUID.
            unsafe { component.getControllerClassId(&mut controller_cid) },
            "getControllerClassId",
        )?;
        controller = create::<IEditController>(factory, &controller_cid)?;
        _controller_init = Some(Initialized::new(
            controller
                .cast()
                .ok_or("IEditController lacks IPluginBase")?,
            host,
        )?);
    }
    let _connection = if _controller_init.is_some() {
        Some(Connection::new(&component, &controller)?)
    } else {
        None
    };
    // SAFETY: Controller is initialized, live, and accessed only from this main thread.
    let count = unsafe { controller.getParameterCount() };
    if !(1..=100_000).contains(&count) {
        return Err(format!("invalid parameter count: {count}"));
    }
    let mut values = Vec::new();
    for index in 0..count {
        // SAFETY: ParameterInfo contains only integers, floats and fixed arrays; all-zero is valid storage.
        let mut info: ParameterInfo = unsafe { mem::zeroed() };
        status(
            // SAFETY: Index is within the returned count and info is writable through the call.
            unsafe { controller.getParameterInfo(index, &mut info) },
            "getParameterInfo",
        )?;
        if !(0.0..=1.0).contains(&info.defaultNormalizedValue) {
            return Err(format!("invalid normalized default for {}", info.id));
        }
        // SAFETY: ID was returned by this controller and normalized endpoints/default are within [0, 1].
        let (min, max, default) = unsafe {
            (
                controller.normalizedParamToPlain(info.id, 0.0),
                controller.normalizedParamToPlain(info.id, 1.0),
                controller.normalizedParamToPlain(info.id, info.defaultNormalizedValue),
            )
        };
        if ![min, max, default, info.defaultNormalizedValue]
            .iter()
            .all(|v| v.is_finite())
        {
            return Err(format!(
                "non-finite parameter range/default for {}",
                info.id
            ));
        }
        let mut display = [[0; 128]; 3];
        for (text, normalized) in display
            .iter_mut()
            .zip([0.0, 1.0, info.defaultNormalizedValue])
        {
            status(
                // SAFETY: Live controller, known parameter ID, valid normalized input and writable String128.
                unsafe { controller.getParamStringByValue(info.id, normalized, text) },
                "getParamStringByValue",
            )?;
        }
        values.push(
            json!({"index": index, "id": info.id, "title": wide(&info.title)?,
            "short_title": wide(&info.shortTitle)?, "units": wide(&info.units)?,
            "step_count": info.stepCount, "default_normalized": info.defaultNormalizedValue,
            "plain_at_normalized_0": min, "plain_at_normalized_1": max, "default_plain": default,
            "display_at_normalized_0": wide(&display[0])?, "display_at_normalized_1": wide(&display[1])?, "default_display": wide(&display[2])?,
            "unit_id": info.unitId, "flags": info.flags}),
        );
    }
    Ok(
        json!({"controller_class_id_bytes": if controller_cid == [0; 16] { Value::Null } else { json!(id_bytes(&controller_cid)) }, "parameters": values}),
    )
}

pub fn dump(path: &Path) -> Result<Value, String> {
    if !cfg!(target_arch = "x86_64") {
        return Err("VST3 binary inspection requires Windows x64".into());
    }
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let isolated = format!("compat-rust-{}-{stamp:x}", std::process::id());
    // SAFETY: This CLI is single-threaded and no native library/COM has been initialized yet.
    // Plug-in constructors may start threads, so every override is set before loading the module.
    unsafe {
        std::env::set_var("HEARASIDE_DEFAULT_BUS", &isolated);
        std::env::set_var("HEARASIDE_SETTINGS_NAME", &isolated);
        std::env::set_var("HEARASIDE_ACCOUNT_NAME", &isolated);
        std::env::set_var("HEARASIDE_NO_TUNNEL", "1");
        std::env::set_var("HEARASIDE_API_URL", "http://127.0.0.1:9");
    }
    let _apartment = Apartment::new()?;
    let module = Module::load(path)?;
    let factory = module.factory()?;
    let host = ComWrapper::new(Host)
        .to_com_ptr::<IHostApplication>()
        .ok_or("cannot create host context")?;
    let context = host.as_ptr().cast::<FUnknown>();
    // SAFETY: PFactoryInfo's fields are fixed integer/character arrays with valid zero values.
    let mut info: PFactoryInfo = unsafe { mem::zeroed() };
    status(
        // SAFETY: Factory remains loaded and info is writable through the call.
        unsafe { factory.getFactoryInfo(&mut info) },
        "getFactoryInfo",
    )?;
    // SAFETY: Factory is live and called from the main thread.
    let count = unsafe { factory.countClasses() };
    if !(0..=10_000).contains(&count) {
        return Err(format!("invalid factory class count: {count}"));
    }
    let factory2 = factory.cast::<IPluginFactory2>();
    let mut classes = Vec::new();
    for index in 0..count {
        // SAFETY: PClassInfo consists only of fixed character arrays and an integer.
        let mut class: PClassInfo = unsafe { mem::zeroed() };
        status(
            // SAFETY: Index is within count and class is writable through the call.
            unsafe { factory.getClassInfo(index, &mut class) },
            "getClassInfo",
        )?;
        let category = narrow(&class.category)?;
        let mut value = json!({"index": index, "class_id_bytes": id_bytes(&class.cid),
            "name": narrow(&class.name)?, "category": category, "cardinality": class.cardinality});
        if let Some(factory2) = &factory2 {
            // SAFETY: PClassInfo2 consists only of fixed arrays and integer fields.
            let mut extended: PClassInfo2 = unsafe { mem::zeroed() };
            status(
                // SAFETY: Same factory index and valid writable metadata storage.
                unsafe { factory2.getClassInfo2(index, &mut extended) },
                "getClassInfo2",
            )?;
            value["subcategories"] = json!(narrow(&extended.subCategories)?);
            value["vendor"] = json!(narrow(&extended.vendor)?);
            value["version"] = json!(narrow(&extended.version)?);
            value["sdk_version"] = json!(narrow(&extended.sdkVersion)?);
            value["class_flags"] = json!(extended.classFlags);
        }
        if category == "Audio Module Class" {
            value["controller"] = parameters(&factory, &class.cid, context)?;
        }
        classes.push(value);
    }
    Ok(
        json!({"schema_version": 1, "format": "VST3", "platform": "windows-x86_64",
        "class_id_encoding": "raw 16-byte TUID in Windows memory order",
        "factory": {"vendor": narrow(&info.vendor)?, "url": narrow(&info.url)?, "email": narrow(&info.email)?, "flags": info.flags},
        "classes": classes}),
    )
}
